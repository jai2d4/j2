#pragma once

#include <filesystem>
#include <string>

#include "core/common/result.h"

namespace trace {

/// Owns the on-disk shape of the TRACE data directory.
///
///     TRACE_DATA/
///       trace.db
///       logs/
///       cases/<case-uuid>/
///         originals/    immutable ingested files
///         working/      analysis-safe derivations
///         thumbnails/
///         exports/
///         reports/
///         logs/
///
/// Paths are stored in the database *relative* to the data root, so a case
/// directory can be moved to another workstation or drive letter without
/// invalidating every row.
class StorageLayout {
public:
    explicit StorageLayout(std::filesystem::path dataRoot);

    /// Platform default, overridable with the TRACE_DATA_DIR environment
    /// variable and, at runtime, by the application settings.
    static std::filesystem::path defaultDataRoot();

    const std::filesystem::path& dataRoot() const { return dataRoot_; }
    std::filesystem::path databasePath() const;
    std::filesystem::path logsDirectory() const;
    std::filesystem::path casesDirectory() const;

    /// Relative path recorded in the cases table.
    static std::string caseRelPath(const std::string& caseId);

    std::filesystem::path caseDirectory(const std::string& caseId) const;
    std::filesystem::path originalsDirectory(const std::string& caseId) const;
    std::filesystem::path workingDirectory(const std::string& caseId) const;
    std::filesystem::path thumbnailsDirectory(const std::string& caseId) const;
    std::filesystem::path exportsDirectory(const std::string& caseId) const;
    std::filesystem::path reportsDirectory(const std::string& caseId) const;
    std::filesystem::path caseLogsDirectory(const std::string& caseId) const;

    /// Creates the data root and its top-level directories.
    Status ensureDataRoot() const;
    /// Creates the full directory set for one case.
    Status ensureCaseDirectories(const std::string& caseId) const;

    /// Turns a stored relative path into an absolute one.
    std::filesystem::path resolve(const std::string& relativePath) const;
    /// Turns an absolute path inside the data root back into a stored relative
    /// path (forward slashes, so databases move between platforms).
    std::string relativeTo(const std::filesystem::path& absolutePath) const;

    /// Collision-safe managed name: the evidence UUID carries identity, the
    /// readable suffix is a courtesy for anyone browsing the folder.
    static std::string managedOriginalFilename(const std::string& evidenceId,
                                               const std::string& originalFilename);
    /// Managed name for a derived asset, e.g.
    /// "EVD-000001_frame_00-00-05-000_<uuid8>.png".
    static std::string derivedFilename(const std::string& evidenceNumber,
                                       const std::string& kind,
                                       const std::string& discriminator,
                                       const std::string& extension,
                                       const std::string& assetId);

private:
    std::filesystem::path dataRoot_;
};

}  // namespace trace
