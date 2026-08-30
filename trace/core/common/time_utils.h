#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace trace {

/// TRACE stores every media position as signed microseconds from the start of
/// the stream. Microseconds are FFmpeg's own AV_TIME_BASE unit, so no precision
/// is lost when converting presentation timestamps, and variable-frame-rate
/// media never has to be forced onto a frame grid.
using Microseconds = std::int64_t;

/// Wall-clock timestamps are persisted as ISO-8601 UTC with millisecond
/// precision (e.g. "2026-08-20T22:31:00.123Z") so they sort lexicographically
/// in SQLite and stay unambiguous across time zones.
std::string nowIso8601Utc();

/// The same format, a number of minutes ahead of now. Used for lockout windows,
/// which are compared as strings — ISO-8601 UTC sorts lexicographically, so a
/// string comparison and a time comparison agree.
std::string iso8601UtcPlusMinutes(int minutes);
std::string toIso8601Utc(std::chrono::system_clock::time_point tp);
std::optional<std::chrono::system_clock::time_point> parseIso8601Utc(const std::string& text);

/// Media timecode helpers: HH:MM:SS.mmm.
std::string formatTimecode(Microseconds micros, bool withMilliseconds = true);
/// Accepts "HH:MM:SS.mmm", "MM:SS.mmm", "SS.mmm" and bare seconds.
std::optional<Microseconds> parseTimecode(const std::string& text);

/// Human-readable duration for inspector panels ("1 min 37.433 s").
std::string formatDurationHuman(Microseconds micros);

}  // namespace trace
