#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/common/logging.h"
#include "core/common/result.h"
#include "core/repositories/settings_repository.h"
#include "core/services/audit_service.h"

namespace trace {

/// Canonical setting keys. Grouped by the subsystem that reads them; keys that
/// belong to capabilities Phase 0 does not implement are marked reserved and
/// their controls are shown disabled rather than pretending to work.
namespace settings_keys {
inline constexpr const char* kDataDirectory = "storage.data_directory";
inline constexpr const char* kThumbnailCacheDirectory = "storage.thumbnail_cache_directory";
inline constexpr const char* kLogLevel = "logging.level";
inline constexpr const char* kPlaybackDefaultSpeed = "playback.default_speed";
inline constexpr const char* kPlaybackVolume = "playback.volume";
inline constexpr const char* kPlaybackMuted = "playback.muted";
inline constexpr const char* kPlaybackJumpSeconds = "playback.jump_seconds";
/// Reserved — Phase 0 always decodes in software.
inline constexpr const char* kHardwareAcceleration = "media.hardware_acceleration";
/// Reserved — no inference runs in Phase 0.
inline constexpr const char* kAnalysisDevice = "ai.device";
inline constexpr const char* kAnalysisCudaDeviceIndex = "ai.cuda_device_index";
}  // namespace settings_keys

/// A setting with its default and whether Phase 0 actually honours it.
struct SettingDefinition {
    std::string key;
    std::string displayName;
    std::string description;
    std::string defaultValue;
    std::string valueType;  ///< string | bool | int | double | path
    bool activeInPhase0 = true;
};

const std::vector<SettingDefinition>& settingDefinitions();
const SettingDefinition* findSettingDefinition(const std::string& key);

/// Typed access to persisted application settings, with audit on change.
class SettingsService {
public:
    SettingsService(std::shared_ptr<Database> database, std::shared_ptr<AuditService> audit);

    /// Writes any missing defaults. Called once at startup.
    Status ensureDefaults();

    std::string getString(const std::string& key, const std::string& fallback = {});
    bool getBool(const std::string& key, bool fallback = false);
    std::int64_t getInt(const std::string& key, std::int64_t fallback = 0);
    double getDouble(const std::string& key, double fallback = 0.0);

    Status setString(const std::string& key, const std::string& value);
    Status setBool(const std::string& key, bool value);
    Status setInt(const std::string& key, std::int64_t value);
    Status setDouble(const std::string& key, double value);

    LogLevel logLevel();

private:
    Status recordChange(const std::string& key, const std::string& oldValue,
                        const std::string& newValue);

    std::shared_ptr<SettingsRepository> repository_;
    std::shared_ptr<AuditService> audit_;
};

}  // namespace trace
