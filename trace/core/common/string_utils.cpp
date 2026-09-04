#include "core/common/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace trace {
namespace {

const std::array<std::string_view, 22> kReservedWindowsNames = {
    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

}  // namespace

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const std::string lowerHaystack = toLower(haystack);
    const std::string lowerNeedle = toLower(needle);
    return lowerHaystack.find(lowerNeedle) != std::string::npos;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += separator;
        out += parts[i];
    }
    return out;
}

std::string sanitizeForFilename(std::string_view text, std::size_t maxLength) {
    std::string out;
    out.reserve(std::min(text.size(), maxLength));
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || byte == 0x7F) {
            out.push_back('_');
        } else if (byte >= 0x80) {
            out.push_back('_');  // keep managed names 7-bit clean across filesystems
        } else {
            out.push_back(c);
        }
        if (out.size() >= maxLength) break;
    }
    // Trailing dots and spaces are illegal on Windows.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) out = "unnamed";

    std::string stem = out;
    const auto dot = stem.find('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    std::string upper;
    upper.reserve(stem.size());
    std::transform(stem.begin(), stem.end(), std::back_inserter(upper),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const auto& reserved : kReservedWindowsNames) {
        if (upper == reserved) {
            out.insert(out.begin(), '_');
            break;
        }
    }
    return out;
}

std::string formatFileSize(std::int64_t bytes) {
    if (bytes < 0) return "Not available";
    static constexpr const char* kUnits[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    auto value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    std::array<char, 48> buffer{};
    if (unit == 0) {
        std::snprintf(buffer.data(), buffer.size(), "%lld bytes", static_cast<long long>(bytes));
    } else {
        std::snprintf(buffer.data(), buffer.size(), "%.2f %s (%lld bytes)", value, kUnits[unit],
                      static_cast<long long>(bytes));
    }
    return std::string(buffer.data());
}

std::string formatSequenceNumber(std::string_view prefix, std::int64_t value, int width) {
    std::string digits = std::to_string(value);
    if (static_cast<int>(digits.size()) < width) {
        digits.insert(digits.begin(), static_cast<std::size_t>(width) - digits.size(), '0');
    }
    return std::string(prefix) + digits;
}

}  // namespace trace
