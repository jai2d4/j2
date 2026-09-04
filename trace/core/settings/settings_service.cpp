#include "core/settings/settings_service.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "core/common/logging.h"
#include "core/common/string_utils.h"
#include "core/security/user_context.h"
#include "core/storage/storage_layout.h"

namespace trace {

const std::vector<SettingDefinition>& settingDefinitions() {
    static const std::vector<SettingDefinition> kDefinitions = {
        {settings_keys::kDataDirectory, "Data directory",
         "Root of managed evidence storage and the case database.",
         StorageLayout::defaultDataRoot().string(), "path", true},
        {settings_keys::kThumbnailCacheDirectory, "Thumbnail cache",
         "Where generated preview images are cached. Empty means per-case storage.", "", "path",
         true},
        {settings_keys::kLogLevel, "Logging level",
         "Verbosity of the application log (trace, debug, info, warn, error).", "info", "string",
         true},
        {settings_keys::kPlaybackDefaultSpeed, "Default playback speed",
         "Speed applied when an evidence item is opened.", "1.0", "double", true},
        {settings_keys::kPlaybackVolume, "Default volume", "Playback volume, 0-100.", "80", "int",
         true},
        {settings_keys::kPlaybackMuted, "Start muted", "Open evidence with audio muted.", "false",
         "bool", true},
        {settings_keys::kPlaybackJumpSeconds, "Jump interval (seconds)",
         "Distance moved by the jump forward/backward controls.", "5", "int", true},
        {settings_keys::kHardwareAcceleration, "Hardware-accelerated decoding",
         "Reserved. Phase 0 decodes in software so that frame timing is exact on every "
         "workstation; GPU decode arrives with the analysis phases.",
         "false", "bool", false},
        {settings_keys::kAnalysisDevice, "Analysis device",
         "Reserved for future analysis providers (cpu, cuda). No inference runs in Phase 0.", "cpu",
         "string", false},
        {settings_keys::kAnalysisCudaDeviceIndex, "CUDA device index",
         "Reserved for future GPU inference.", "0", "int", false},
    };
    return kDefinitions;
}

const SettingDefinition* findSettingDefinition(const std::string& key) {
    const auto& definitions = settingDefinitions();
    const auto it = std::find_if(definitions.begin(), definitions.end(),
                                 [&](const SettingDefinition& d) { return d.key == key; });
    return it == definitions.end() ? nullptr : &(*it);
}

SettingsService::SettingsService(std::shared_ptr<Database> database,
                                 std::shared_ptr<AuditService> audit)
    : repository_(std::make_shared<SettingsRepository>(std::move(database))),
      audit_(std::move(audit)) {}

Status SettingsService::ensureDefaults() {
    for (const auto& definition : settingDefinitions()) {
        auto existing = repository_->get(definition.key);
        if (!existing) return Status(existing.error());
        if (existing.value().has_value()) continue;
        if (auto status = repository_->set(definition.key, definition.defaultValue,
                                           definition.valueType, "system");
            !status) {
            return status;
        }
    }
    return Status::success();
}

std::string SettingsService::getString(const std::string& key, const std::string& fallback) {
    auto value = repository_->get(key);
    if (value && value.value().has_value()) return *value.value();
    if (const auto* definition = findSettingDefinition(key); definition != nullptr) {
        return definition->defaultValue;
    }
    return fallback;
}

bool SettingsService::getBool(const std::string& key, bool fallback) {
    const std::string value = toLower(getString(key, fallback ? "true" : "false"));
    if (value == "true" || value == "1" || value == "yes") return true;
    if (value == "false" || value == "0" || value == "no") return false;
    return fallback;
}

std::int64_t SettingsService::getInt(const std::string& key, std::int64_t fallback) {
    const std::string value = getString(key, std::to_string(fallback));
    try {
        return std::stoll(value);
    } catch (const std::exception&) {
        logWarn("settings", "Setting is not an integer; using the fallback",
                JsonValue::object().set("key", key).set("value", value));
        return fallback;
    }
}

double SettingsService::getDouble(const std::string& key, double fallback) {
    const std::string value = getString(key, std::to_string(fallback));
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        logWarn("settings", "Setting is not a number; using the fallback",
                JsonValue::object().set("key", key).set("value", value));
        return fallback;
    }
}

Status SettingsService::recordChange(const std::string& key, const std::string& oldValue,
                                     const std::string& newValue) {
    if (oldValue == newValue) return Status::success();
    AuditRecord record;
    record.action = AuditAction::SettingsChanged;
    record.description = "Setting changed: " + key;
    record.details = JsonValue::object()
                         .set("key", key)
                         .set("previous_value", oldValue)
                         .set("new_value", newValue);
    auto audited = audit_->record(record);
    if (!audited) return Status(audited.error());
    return Status::success();
}

Status SettingsService::setString(const std::string& key, const std::string& value) {
    if (!UserContext::current().can(Permission::ManageSettings)) {
        return Status::failure(ErrorCode::PermissionDenied,
                               "You do not have permission to change settings");
    }
    const std::string previous = getString(key);
    const auto* definition = findSettingDefinition(key);
    const std::string valueType = definition != nullptr ? definition->valueType : "string";
    if (auto status =
            repository_->set(key, value, valueType, UserContext::current().actorName());
        !status) {
        return status;
    }
    return recordChange(key, previous, value);
}

Status SettingsService::setBool(const std::string& key, bool value) {
    return setString(key, value ? "true" : "false");
}
Status SettingsService::setInt(const std::string& key, std::int64_t value) {
    return setString(key, std::to_string(value));
}
Status SettingsService::setDouble(const std::string& key, double value) {
    return setString(key, std::to_string(value));
}

LogLevel SettingsService::logLevel() {
    return logLevelFromString(getString(settings_keys::kLogLevel, "info"), LogLevel::Info);
}

}  // namespace trace
