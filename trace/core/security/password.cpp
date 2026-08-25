#include "core/security/password.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "core/security/sha256.h"

namespace trace::password {
namespace {

constexpr std::size_t kBlockSize = 64;  // SHA-256 block size, in bytes

std::vector<std::uint8_t> sha256Bytes(const std::vector<std::uint8_t>& data) {
    Sha256 hasher;
    hasher.update(data.data(), data.size());
    const auto digest = hasher.finalizeBytes();
    return {digest.begin(), digest.end()};
}

std::string toHex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

/// Returns an empty vector for anything that is not valid hex, so a corrupted
/// stored record fails verification instead of being silently read as zeroes.
std::vector<std::uint8_t> fromHex(const std::string& text) {
    if (text.size() % 2 != 0) return {};
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        int high = -1;
        int low = -1;
        for (int pass = 0; pass < 2; ++pass) {
            const char c = text[i + static_cast<std::size_t>(pass)];
            int value = -1;
            if (c >= '0' && c <= '9') value = c - '0';
            else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
            if (value < 0) return {};
            if (pass == 0) high = value; else low = value;
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

}  // namespace

std::vector<std::uint8_t> hmacSha256(const std::vector<std::uint8_t>& key,
                                     const std::vector<std::uint8_t>& message) {
    // RFC 2104: a key longer than the block size is replaced by its own hash;
    // a shorter one is zero-padded up to the block size.
    std::vector<std::uint8_t> block(kBlockSize, 0);
    if (key.size() > kBlockSize) {
        const auto hashed = sha256Bytes(key);
        std::copy(hashed.begin(), hashed.end(), block.begin());
    } else {
        std::copy(key.begin(), key.end(), block.begin());
    }

    std::vector<std::uint8_t> inner(kBlockSize);
    std::vector<std::uint8_t> outer(kBlockSize);
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        inner[i] = static_cast<std::uint8_t>(block[i] ^ 0x36);
        outer[i] = static_cast<std::uint8_t>(block[i] ^ 0x5C);
    }

    Sha256 innerHash;
    innerHash.update(inner.data(), inner.size());
    innerHash.update(message.data(), message.size());
    const auto innerDigest = innerHash.finalizeBytes();

    Sha256 outerHash;
    outerHash.update(outer.data(), outer.size());
    outerHash.update(innerDigest.data(), innerDigest.size());
    const auto digest = outerHash.finalizeBytes();
    return {digest.begin(), digest.end()};
}

std::vector<std::uint8_t> pbkdf2Sha256(const std::string& password,
                                       const std::vector<std::uint8_t>& salt,
                                       std::uint32_t iterations, std::size_t keyLength) {
    const std::vector<std::uint8_t> key(password.begin(), password.end());
    std::vector<std::uint8_t> derived;
    derived.reserve(keyLength);

    // RFC 8018 §5.2: the key is the concatenation of blocks T_1..T_n, where each
    // T_i folds `iterations` HMACs together. The whole cost of the function is
    // this loop, and that cost is the point.
    for (std::uint32_t blockIndex = 1; derived.size() < keyLength; ++blockIndex) {
        std::vector<std::uint8_t> message = salt;
        message.push_back(static_cast<std::uint8_t>((blockIndex >> 24) & 0xFF));
        message.push_back(static_cast<std::uint8_t>((blockIndex >> 16) & 0xFF));
        message.push_back(static_cast<std::uint8_t>((blockIndex >> 8) & 0xFF));
        message.push_back(static_cast<std::uint8_t>(blockIndex & 0xFF));

        std::vector<std::uint8_t> u = hmacSha256(key, message);
        std::vector<std::uint8_t> block = u;
        for (std::uint32_t round = 1; round < iterations; ++round) {
            u = hmacSha256(key, u);
            for (std::size_t i = 0; i < block.size(); ++i) block[i] ^= u[i];
        }

        const std::size_t take = std::min(block.size(), keyLength - derived.size());
        derived.insert(derived.end(), block.begin(), block.begin() + static_cast<long>(take));
    }
    return derived;
}

Result<std::vector<std::uint8_t>> randomBytes(std::size_t count) {
    using ResultType = Result<std::vector<std::uint8_t>>;
    std::vector<std::uint8_t> out(count, 0);

#if defined(_WIN32)
    // BCryptGenRandom is the documented CSPRNG on Windows. Declared here rather
    // than pulling <windows.h> into a core header.
    extern "C" long __stdcall BCryptGenRandom(void*, unsigned char*, unsigned long, unsigned long);
    constexpr unsigned long kUseSystemPreferredRng = 0x00000002;
    const long status = BCryptGenRandom(nullptr, out.data(),
                                        static_cast<unsigned long>(count), kUseSystemPreferredRng);
    if (status != 0) {
        return ResultType::failure(ErrorCode::Internal,
                                   "The system random number generator is unavailable");
    }
    return ResultType::success(std::move(out));
#else
    // /dev/urandom is the kernel CSPRNG. There is deliberately no fallback to
    // std::random_device or a time seed: a salt or key from a predictable source
    // would look like security while providing none.
    std::ifstream source("/dev/urandom", std::ios::binary);
    if (!source) {
        return ResultType::failure(ErrorCode::Internal,
                                   "The system random number generator is unavailable");
    }
    source.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count));
    if (!source || static_cast<std::size_t>(source.gcount()) != count) {
        return ResultType::failure(ErrorCode::Internal,
                                   "The system random number generator returned too few bytes");
    }
    return ResultType::success(std::move(out));
#endif
}

Result<StoredPassword> hash(const std::string& plaintext, std::uint32_t iterations) {
    using ResultType = Result<StoredPassword>;

    auto strength = checkStrength(plaintext);
    if (!strength) return ResultType(strength.error());

    auto salt = randomBytes(kSaltBytes);
    if (!salt) return ResultType(salt.error());

    StoredPassword stored;
    stored.algorithm = kAlgorithmPbkdf2Sha256;
    stored.iterations = iterations;
    stored.saltHex = toHex(salt.value());
    stored.hashHex = toHex(pbkdf2Sha256(plaintext, salt.value(), iterations, kKeyBytes));
    return ResultType::success(std::move(stored));
}

bool constantTimeEquals(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    // The lengths are compared openly — they are not secret — but every byte of
    // equal-length inputs is examined, so the time taken says nothing about how
    // far a wrong guess got.
    if (a.size() != b.size()) return false;
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference = static_cast<std::uint8_t>(difference | (a[i] ^ b[i]));
    }
    return difference == 0;
}

bool verify(const std::string& plaintext, const StoredPassword& stored) {
    // Fails closed on anything unrecognised: an unknown algorithm, an
    // unreasonable work factor, or a record whose hex does not parse.
    if (stored.algorithm != kAlgorithmPbkdf2Sha256) return false;
    if (stored.iterations == 0) return false;

    const auto salt = fromHex(stored.saltHex);
    const auto expected = fromHex(stored.hashHex);
    if (salt.empty() || expected.empty()) return false;

    const auto actual = pbkdf2Sha256(plaintext, salt, stored.iterations, expected.size());
    return constantTimeEquals(actual, expected);
}

bool needsRehash(const StoredPassword& stored, std::uint32_t currentIterations) {
    if (stored.algorithm != kAlgorithmPbkdf2Sha256) return true;
    return stored.iterations < currentIterations;
}

Status checkStrength(const std::string& plaintext) {
    if (plaintext.size() < kMinimumLength) {
        return Status::failure(
            ErrorCode::InvalidArgument,
            "A password must be at least " + std::to_string(kMinimumLength) +
                " characters. Length is what makes a password hard to guess; a long "
                "phrase you can remember is stronger than a short one full of symbols.");
    }
    // No composition rules. Requiring a capital, a digit and a symbol reliably
    // produces Password1! — it narrows the search space an attacker has to try
    // rather than widening it.
    return Status::success();
}

}  // namespace trace::password
