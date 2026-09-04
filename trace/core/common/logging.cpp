#include "core/common/logging.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "core/common/time_utils.h"

namespace trace {
namespace {

std::string fieldsToText(const JsonValue& fields) {
    if (!fields.isObject() || fields.members().empty()) return {};
    std::string out;
    for (const auto& [key, value] : fields.members()) {
        out.push_back(' ');
        out += key;
        out.push_back('=');
        if (value.type() == JsonValue::Type::String) {
            const std::string& text = value.stringValue();
            const bool needsQuotes = text.empty() || text.find(' ') != std::string::npos;
            out += needsQuotes ? ("\"" + text + "\"") : text;
        } else {
            out += value.dump();
        }
    }
    return out;
}

}  // namespace

struct Logger::Impl {
    std::mutex mutex;
    std::atomic<LogLevel> level{LogLevel::Info};
    std::atomic<bool> console{true};
    std::ofstream file;
    std::filesystem::path path;
};

Logger::Impl& Logger::impl() {
    static Impl impl;
    return impl;
}

Logger::~Logger() = default;

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

const char* toString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "TRACE";
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warn:     return "WARN";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRIT";
        case LogLevel::Off:      return "OFF";
    }
    return "?";
}

LogLevel logLevelFromString(std::string_view text, LogLevel fallback) {
    if (text == "trace" || text == "TRACE") return LogLevel::Trace;
    if (text == "debug" || text == "DEBUG") return LogLevel::Debug;
    if (text == "info" || text == "INFO") return LogLevel::Info;
    if (text == "warn" || text == "WARN" || text == "warning") return LogLevel::Warn;
    if (text == "error" || text == "ERROR") return LogLevel::Error;
    if (text == "critical" || text == "CRIT") return LogLevel::Critical;
    if (text == "off" || text == "OFF") return LogLevel::Off;
    return fallback;
}

void Logger::setLevel(LogLevel level) { impl().level.store(level); }
LogLevel Logger::level() const { return const_cast<Logger*>(this)->impl().level.load(); }
void Logger::setConsoleEnabled(bool enabled) { impl().console.store(enabled); }

bool Logger::setLogDirectory(const std::filesystem::path& directory) {
    auto& state = impl();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return false;

    const std::string stamp = nowIso8601Utc().substr(0, 10);  // YYYY-MM-DD
    std::string name = "trace-";
    for (char c : stamp) {
        if (c != '-') name.push_back(c);
    }
    name += ".log";

    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.file.is_open()) state.file.close();
    state.path = directory / name;
    state.file.open(state.path, std::ios::app);
    return state.file.is_open();
}

std::filesystem::path Logger::logFilePath() const { return const_cast<Logger*>(this)->impl().path; }

void Logger::write(LogLevel level, std::string_view component, std::string_view message,
                   const JsonValue& fields) {
    auto& state = impl();
    if (level < state.level.load() || state.level.load() == LogLevel::Off) return;

    std::ostringstream line;
    line << nowIso8601Utc() << ' ' << toString(level) << " [" << component << "] " << message
         << fieldsToText(fields);
    const std::string text = line.str();

    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.console.load()) {
        std::fputs(text.c_str(), level >= LogLevel::Warn ? stderr : stdout);
        std::fputc('\n', level >= LogLevel::Warn ? stderr : stdout);
    }
    if (state.file.is_open()) {
        state.file << text << '\n';
        state.file.flush();
    }
}

void Logger::shutdown() {
    auto& state = impl();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.file.is_open()) {
        state.file.flush();
        state.file.close();
    }
}

void logTrace(std::string_view c, std::string_view m, const JsonValue& f) { Logger::instance().write(LogLevel::Trace, c, m, f); }
void logDebug(std::string_view c, std::string_view m, const JsonValue& f) { Logger::instance().write(LogLevel::Debug, c, m, f); }
void logInfo(std::string_view c, std::string_view m, const JsonValue& f) { Logger::instance().write(LogLevel::Info, c, m, f); }
void logWarn(std::string_view c, std::string_view m, const JsonValue& f) { Logger::instance().write(LogLevel::Warn, c, m, f); }
void logError(std::string_view c, std::string_view m, const JsonValue& f) { Logger::instance().write(LogLevel::Error, c, m, f); }
void logCritical(std::string_view c, std::string_view m, const JsonValue& f) { Logger::instance().write(LogLevel::Critical, c, m, f); }

}  // namespace trace
