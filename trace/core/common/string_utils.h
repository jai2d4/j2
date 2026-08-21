#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace trace {

std::string trim(std::string_view text);
std::string toLower(std::string_view text);
bool startsWith(std::string_view text, std::string_view prefix);
bool endsWith(std::string_view text, std::string_view suffix);
bool containsCaseInsensitive(std::string_view haystack, std::string_view needle);
std::string join(const std::vector<std::string>& parts, std::string_view separator);

/// Strips path separators, control characters and platform-reserved characters
/// so a user-supplied name can never escape managed storage or collide with a
/// device name on Windows. Never used as identity — only as a readable suffix.
std::string sanitizeForFilename(std::string_view text, std::size_t maxLength = 64);

/// "1.4 GB", "812 KB" — display only.
std::string formatFileSize(std::int64_t bytes);

/// Zero-padded display numbers such as CASE-0001 / EVD-000001.
std::string formatSequenceNumber(std::string_view prefix, std::int64_t value, int width);

}  // namespace trace
