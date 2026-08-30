#include "core/common/time_utils.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <vector>

namespace trace {
namespace {

std::tm gmTime(std::time_t t) {
    std::tm out{};
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

std::time_t timegmPortable(std::tm& tm) {
#if defined(_WIN32)
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

}  // namespace

std::string toIso8601Utc(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto secs = time_point_cast<seconds>(tp);
    const auto millis = duration_cast<milliseconds>(tp - secs).count();
    const std::time_t t = system_clock::to_time_t(secs);
    const std::tm tm = gmTime(t);

    std::array<char, 48> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(millis));
    return std::string(buffer.data());
}

std::string nowIso8601Utc() { return toIso8601Utc(std::chrono::system_clock::now()); }

std::optional<std::chrono::system_clock::time_point> parseIso8601Utc(const std::string& text) {
    if (text.size() < 19) return std::nullopt;
    std::tm tm{};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int millis = 0;
    const int matched = std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day,
                                    &hour, &minute, &second);
    if (matched != 6) return std::nullopt;
    const auto dot = text.find('.');
    if (dot != std::string::npos && dot + 1 < text.size()) {
        std::string frac;
        for (std::size_t i = dot + 1; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) {
            frac.push_back(text[i]);
        }
        frac.resize(3, '0');
        millis = std::atoi(frac.c_str());
    }
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    const std::time_t t = timegmPortable(tm);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    return std::chrono::system_clock::from_time_t(t) + std::chrono::milliseconds(millis);
}

std::string iso8601UtcPlusMinutes(int minutes) {
    // Formatted by the same function as nowIso8601Utc, so the two are directly
    // comparable as strings. A lockout expiry written in a different format
    // would compare wrongly against "now" in the second they share.
    return toIso8601Utc(std::chrono::system_clock::now() + std::chrono::minutes(minutes));
}

std::string formatTimecode(Microseconds micros, bool withMilliseconds) {
    const bool negative = micros < 0;
    std::int64_t value = negative ? -micros : micros;

    const std::int64_t totalMillis = value / 1000;
    const std::int64_t ms = totalMillis % 1000;
    const std::int64_t totalSeconds = totalMillis / 1000;
    const std::int64_t seconds = totalSeconds % 60;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t hours = totalSeconds / 3600;

    std::array<char, 48> buffer{};
    if (withMilliseconds) {
        std::snprintf(buffer.data(), buffer.size(), "%s%02lld:%02lld:%02lld.%03lld",
                      negative ? "-" : "", static_cast<long long>(hours),
                      static_cast<long long>(minutes), static_cast<long long>(seconds),
                      static_cast<long long>(ms));
    } else {
        std::snprintf(buffer.data(), buffer.size(), "%s%02lld:%02lld:%02lld", negative ? "-" : "",
                      static_cast<long long>(hours), static_cast<long long>(minutes),
                      static_cast<long long>(seconds));
    }
    return std::string(buffer.data());
}

std::optional<Microseconds> parseTimecode(const std::string& text) {
    if (text.empty()) return std::nullopt;

    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == ':') {
            parts.push_back(current);
            current.clear();
        } else if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    if (parts.size() > 3) return std::nullopt;

    double seconds = 0.0;
    std::int64_t minutes = 0;
    std::int64_t hours = 0;
    try {
        std::size_t consumed = 0;
        seconds = std::stod(parts.back(), &consumed);
        if (consumed != parts.back().size()) return std::nullopt;
        if (parts.size() >= 2) minutes = std::stoll(parts[parts.size() - 2]);
        if (parts.size() == 3) hours = std::stoll(parts[0]);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (seconds < 0 || minutes < 0 || hours < 0) return std::nullopt;

    const double total = static_cast<double>(hours) * 3600.0 + static_cast<double>(minutes) * 60.0 + seconds;
    return static_cast<Microseconds>(std::llround(total * 1'000'000.0));
}

std::string formatDurationHuman(Microseconds micros) {
    if (micros < 0) return "Not available";
    const double seconds = static_cast<double>(micros) / 1'000'000.0;
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    if (seconds < 60.0) {
        out << seconds << " s";
        return out.str();
    }
    const std::int64_t totalSeconds = micros / 1'000'000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const double remaining = seconds - static_cast<double>(hours * 3600 + minutes * 60);
    if (hours > 0) out << hours << " h ";
    out << minutes << " min " << remaining << " s";
    return out.str();
}

}  // namespace trace
