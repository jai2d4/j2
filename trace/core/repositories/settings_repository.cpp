#include "core/repositories/settings_repository.h"

#include <utility>

#include "core/common/time_utils.h"

namespace trace {

SettingsRepository::SettingsRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Status SettingsRepository::set(const std::string& key, const std::string& value,
                               const std::string& valueType, const std::string& updatedBy) {
    auto prepared = database_->prepare(
        "INSERT INTO application_settings (key, value, value_type, updated_at, updated_by) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, value_type = excluded.value_type, "
        "updated_at = excluded.updated_at, updated_by = excluded.updated_by;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, key).bind(2, value).bind(3, valueType).bind(4, nowIso8601Utc()).bind(5, updatedBy);
    return stmt.run();
}

Result<std::optional<std::string>> SettingsRepository::get(const std::string& key) {
    using ResultType = Result<std::optional<std::string>>;
    auto prepared = database_->prepare("SELECT value FROM application_settings WHERE key = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, key);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(stmt.columnText(0));
}

Result<std::map<std::string, std::string>> SettingsRepository::getAll() {
    using ResultType = Result<std::map<std::string, std::string>>;
    auto prepared = database_->prepare("SELECT key, value FROM application_settings ORDER BY key;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();

    std::map<std::string, std::string> values;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        values.emplace(stmt.columnText(0), stmt.columnText(1));
    }
    return ResultType::success(std::move(values));
}

Status SettingsRepository::remove(const std::string& key) {
    auto prepared = database_->prepare("DELETE FROM application_settings WHERE key = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, key);
    return stmt.run();
}

}  // namespace trace
