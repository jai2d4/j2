#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/common/json.h"

namespace trace {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Critical, Off };

const char* toString(LogLevel level);
LogLevel logLevelFromString(std::string_view text, LogLevel fallback = LogLevel::Info);

/// Process-wide structured logger.
///
/// Records are written as `<iso-timestamp> <LEVEL> [component] message key=value`
/// to the console and, once configured, to a rolling per-day file under the
/// TRACE data directory. Evidence *contents* are never logged: paths, sizes,
/// identifiers and digests are, because those are what incident diagnosis
/// needs, but decoded frames and file bodies are not.
class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel level() const;

    /// Opens (and creates) `directory`/trace-YYYYMMDD.log in append mode.
    bool setLogDirectory(const std::filesystem::path& directory);
    std::filesystem::path logFilePath() const;

    void setConsoleEnabled(bool enabled);

    void write(LogLevel level, std::string_view component, std::string_view message,
               const JsonValue& fields = JsonValue());

    /// Flushes and closes the log file — called from the shutdown path.
    void shutdown();

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    struct Impl;
    Impl& impl();
};

void logTrace(std::string_view component, std::string_view message, const JsonValue& fields = JsonValue());
void logDebug(std::string_view component, std::string_view message, const JsonValue& fields = JsonValue());
void logInfo(std::string_view component, std::string_view message, const JsonValue& fields = JsonValue());
void logWarn(std::string_view component, std::string_view message, const JsonValue& fields = JsonValue());
void logError(std::string_view component, std::string_view message, const JsonValue& fields = JsonValue());
void logCritical(std::string_view component, std::string_view message, const JsonValue& fields = JsonValue());

}  // namespace trace
