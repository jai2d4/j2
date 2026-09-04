#include "core/common/uuid.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <random>

namespace trace {
namespace {

std::mt19937_64& engine() {
    // Seeded once per process from the platform entropy source; guarded because
    // ingestion, UI and worker threads all mint identifiers. The seed sequence
    // is handed to the engine directly — never copied through a differently
    // typed pointer, which the optimiser is free to elide.
    static std::mt19937_64 gen{[] {
        std::random_device source;
        std::seed_seq sequence{source(), source(), source(), source(),
                               source(), source(), source(), source()};
        return std::mt19937_64(sequence);
    }()};
    return gen;
}

std::mutex& engineMutex() {
    static std::mutex m;
    return m;
}

char hexDigit(unsigned value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    return kDigits[value & 0xFu];
}

}  // namespace

namespace detail {

std::uint64_t generateEntropySeed() {
    std::random_device source;
    const auto high = static_cast<std::uint64_t>(source());
    const auto low = static_cast<std::uint64_t>(source());
    return (high << 32) | low;
}

}  // namespace detail

std::string generateUuid() {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    {
        std::lock_guard<std::mutex> lock(engineMutex());
        hi = engine()();
        lo = engine()();
    }
    // RFC 4122 version 4, variant 1.
    hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

    std::string out;
    out.reserve(36);
    auto appendBytes = [&out](std::uint64_t value, int bytes) {
        for (int i = bytes - 1; i >= 0; --i) {
            const auto byte = static_cast<unsigned>((value >> (i * 8)) & 0xFFu);
            out.push_back(hexDigit(byte >> 4));
            out.push_back(hexDigit(byte));
        }
    };
    appendBytes(hi >> 32, 4);
    out.push_back('-');
    appendBytes((hi >> 16) & 0xFFFFull, 2);
    out.push_back('-');
    appendBytes(hi & 0xFFFFull, 2);
    out.push_back('-');
    appendBytes(lo >> 48, 2);
    out.push_back('-');
    appendBytes(lo & 0xFFFFFFFFFFFFull, 6);
    return out;
}

bool isUuid(const std::string& text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

std::string uuidToCompact(const std::string& uuid) {
    std::string out;
    out.reserve(32);
    for (char c : uuid) {
        if (c != '-') out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace trace
