#include "core/storage/storage_layout.h"

#include <cstdlib>
#include <utility>

#include "core/common/string_utils.h"
#include "core/common/uuid.h"

namespace trace {
namespace {

std::filesystem::path homeDirectory() {
#if defined(_WIN32)
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr) return profile;
#else
    if (const char* home = std::getenv("HOME"); home != nullptr) return home;
#endif
    return std::filesystem::current_path();
}

Status createDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return Status::failure(ErrorCode::IoError, "Unable to create directory: " + path.string(),
                               ec.message());
    }
    return Status::success();
}

}  // namespace

StorageLayout::StorageLayout(std::filesystem::path dataRoot) : dataRoot_(std::move(dataRoot)) {}

std::filesystem::path StorageLayout::defaultDataRoot() {
    if (const char* override = std::getenv("TRACE_DATA_DIR"); override != nullptr && *override != '\0') {
        return std::filesystem::path(override);
    }
#if defined(_WIN32)
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData != nullptr) {
        return std::filesystem::path(localAppData) / "TRACE" / "TRACE_DATA";
    }
    return homeDirectory() / "TRACE" / "TRACE_DATA";
#elif defined(__APPLE__)
    return homeDirectory() / "Library" / "Application Support" / "TRACE" / "TRACE_DATA";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "TRACE" / "TRACE_DATA";
    }
    return homeDirectory() / ".local" / "share" / "TRACE" / "TRACE_DATA";
#endif
}

std::filesystem::path StorageLayout::databasePath() const { return dataRoot_ / "trace.db"; }
std::filesystem::path StorageLayout::logsDirectory() const { return dataRoot_ / "logs"; }
std::filesystem::path StorageLayout::casesDirectory() const { return dataRoot_ / "cases"; }

std::string StorageLayout::caseRelPath(const std::string& caseId) { return "cases/" + caseId; }

std::filesystem::path StorageLayout::caseDirectory(const std::string& caseId) const {
    return casesDirectory() / caseId;
}
std::filesystem::path StorageLayout::originalsDirectory(const std::string& caseId) const {
    return caseDirectory(caseId) / "originals";
}
std::filesystem::path StorageLayout::workingDirectory(const std::string& caseId) const {
    return caseDirectory(caseId) / "working";
}
std::filesystem::path StorageLayout::thumbnailsDirectory(const std::string& caseId) const {
    return caseDirectory(caseId) / "thumbnails";
}
std::filesystem::path StorageLayout::exportsDirectory(const std::string& caseId) const {
    return caseDirectory(caseId) / "exports";
}
std::filesystem::path StorageLayout::reportsDirectory(const std::string& caseId) const {
    return caseDirectory(caseId) / "reports";
}
std::filesystem::path StorageLayout::caseLogsDirectory(const std::string& caseId) const {
    return caseDirectory(caseId) / "logs";
}

Status StorageLayout::ensureDataRoot() const {
    if (auto status = createDirectory(dataRoot_); !status) return status;
    if (auto status = createDirectory(logsDirectory()); !status) return status;
    return createDirectory(casesDirectory());
}

Status StorageLayout::ensureCaseDirectories(const std::string& caseId) const {
    if (auto status = ensureDataRoot(); !status) return status;
    for (const auto& directory :
         {originalsDirectory(caseId), workingDirectory(caseId), thumbnailsDirectory(caseId),
          exportsDirectory(caseId), reportsDirectory(caseId), caseLogsDirectory(caseId)}) {
        if (auto status = createDirectory(directory); !status) return status;
    }
    return Status::success();
}

std::filesystem::path StorageLayout::resolve(const std::string& relativePath) const {
    return dataRoot_ / std::filesystem::path(relativePath);
}

std::string StorageLayout::relativeTo(const std::filesystem::path& absolutePath) const {
    std::error_code ec;
    const auto relative = std::filesystem::relative(absolutePath, dataRoot_, ec);
    if (ec || relative.empty()) return absolutePath.generic_string();
    return relative.generic_string();
}

std::string StorageLayout::managedOriginalFilename(const std::string& evidenceId,
                                                   const std::string& originalFilename) {
    const std::filesystem::path original(originalFilename);
    const std::string stem = sanitizeForFilename(original.stem().string(), 60);
    std::string extension = sanitizeForFilename(original.extension().string(), 16);
    if (!extension.empty() && extension.front() != '.') extension.insert(extension.begin(), '.');
    return uuidToCompact(evidenceId) + "_" + stem + extension;
}

std::string StorageLayout::derivedFilename(const std::string& evidenceNumber,
                                           const std::string& kind,
                                           const std::string& discriminator,
                                           const std::string& extension,
                                           const std::string& assetId) {
    std::string name = sanitizeForFilename(evidenceNumber, 24) + "_" + sanitizeForFilename(kind, 24);
    if (!discriminator.empty()) name += "_" + sanitizeForFilename(discriminator, 32);
    name += "_" + uuidToCompact(assetId).substr(0, 8);
    std::string ext = sanitizeForFilename(extension, 16);
    if (!ext.empty() && ext.front() != '.') ext.insert(ext.begin(), '.');
    return name + ext;
}

}  // namespace trace
