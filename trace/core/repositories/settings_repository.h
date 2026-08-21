#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "core/common/result.h"
#include "core/database/database.h"

namespace trace {

/// Key/value application settings persisted alongside the cases.
///
/// Values are stored as text with a declared type so the UI can render the
/// right editor and so a malformed value is a recoverable parse error rather
/// than a crash.
class SettingsRepository {
public:
    explicit SettingsRepository(std::shared_ptr<Database> database);

    Status set(const std::string& key, const std::string& value,
               const std::string& valueType = "string", const std::string& updatedBy = {});
    Result<std::optional<std::string>> get(const std::string& key);
    Result<std::map<std::string, std::string>> getAll();
    Status remove(const std::string& key);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
