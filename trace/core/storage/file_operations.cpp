#include "core/storage/file_operations.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/common/logging.h"
#include "core/common/time_utils.h"
#include "core/security/sha256.h"

namespace trace {
namespace {
constexpr const char* kComponent = "storage";
}

Result<CopyOutcome> copyIntoManagedStorage(const std::filesystem::path& source,
                                           const std::filesystem::path& destination,
                                           const ProgressCallback& progress) {
    using ResultType = Result<CopyOutcome>;
    std::error_code ec;

    if (!std::filesystem::exists(source, ec)) {
        return ResultType::failure(ErrorCode::NotFound, "Source file not found: " + source.string());
    }
    if (std::filesystem::is_directory(source, ec)) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "Source is a directory: " + source.string());
    }
    if (std::filesystem::exists(destination, ec)) {
        // Managed names embed a fresh UUID, so this means something is wrong
        // with the caller — never silently overwrite stored evidence.
        return ResultType::failure(ErrorCode::AlreadyExists,
                                   "Managed storage already contains: " + destination.string());
    }

    const auto totalBytes = std::filesystem::file_size(source, ec);
    if (ec) {
        return ResultType::failure(ErrorCode::IoError,
                                   "Unable to determine source size: " + source.string(), ec.message());
    }

    std::ifstream input(source, std::ios::binary);
    if (!input) {
        return ResultType::failure(ErrorCode::IoError,
                                   "Unable to read source file: " + source.string(),
                                   std::strerror(errno));
    }
    std::filesystem::create_directories(destination.parent_path(), ec);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        return ResultType::failure(ErrorCode::IoError,
                                   "Unable to create managed copy: " + destination.string(),
                                   std::strerror(errno));
    }

    Sha256 hasher;
    std::vector<char> buffer(kStreamChunkSize);
    CopyOutcome outcome;
    HashProgress state{0, static_cast<std::int64_t>(totalBytes)};

    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read <= 0) break;

        output.write(buffer.data(), read);
        if (!output) {
            output.close();
            removeManagedFile(destination);
            return ResultType::failure(ErrorCode::IoError,
                                       "Write failed while copying into managed storage",
                                       std::strerror(errno));
        }
        hasher.update(buffer.data(), static_cast<std::size_t>(read));
        outcome.bytesCopied += read;
        state.bytesProcessed = outcome.bytesCopied;

        if (progress && !progress(state)) {
            output.close();
            removeManagedFile(destination);
            return ResultType::failure(ErrorCode::Cancelled, "Import cancelled by operator");
        }
    }

    if (input.bad()) {
        output.close();
        removeManagedFile(destination);
        return ResultType::failure(ErrorCode::IoError, "Read failed while copying source file",
                                   std::strerror(errno));
    }

    output.flush();
    if (!output) {
        output.close();
        removeManagedFile(destination);
        return ResultType::failure(ErrorCode::IoError, "Unable to flush managed copy to disk",
                                   std::strerror(errno));
    }
    output.close();
    outcome.sourceSha256 = hasher.finalizeHex();

    const auto writtenSize = std::filesystem::file_size(destination, ec);
    if (ec || writtenSize != totalBytes) {
        removeManagedFile(destination);
        return ResultType::failure(ErrorCode::IoError,
                                   "Managed copy is not the same size as the source",
                                   "expected " + std::to_string(totalBytes) + " bytes, wrote " +
                                       std::to_string(ec ? 0 : writtenSize));
    }

    // Independent verification pass over what actually landed on disk.
    auto destinationHash = hashFile(destination, progress);
    if (!destinationHash) {
        removeManagedFile(destination);
        return ResultType(destinationHash.error());
    }
    outcome.destinationSha256 = destinationHash.take();

    if (outcome.destinationSha256 != outcome.sourceSha256) {
        removeManagedFile(destination);
        logError(kComponent, "Managed copy did not match the source digest",
                 JsonValue::object()
                     .set("source", source.string())
                     .set("source_sha256", outcome.sourceSha256)
                     .set("copy_sha256", outcome.destinationSha256));
        return ResultType::failure(ErrorCode::IntegrityFailure,
                                   "The copy of the evidence did not match the source file",
                                   "source " + outcome.sourceSha256 + " vs copy " +
                                       outcome.destinationSha256);
    }

    return ResultType::success(std::move(outcome));
}

std::optional<std::string> fileModifiedTimeIso8601(const std::filesystem::path& path) {
    std::error_code ec;
    const auto fileTime = std::filesystem::last_write_time(path, ec);
    if (ec) return std::nullopt;

    // file_clock's epoch is unspecified; convert through the portable route.
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return toIso8601Utc(systemTime);
}

bool setReadOnly(const std::filesystem::path& path) {
    std::error_code ec;
    auto permissions = std::filesystem::status(path, ec).permissions();
    if (ec) return false;
    permissions &= ~(std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                     std::filesystem::perms::others_write);
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, ec);
    return !ec;
}

bool removeManagedFile(const std::filesystem::path& path) {
    std::error_code ec;
    // Rollback may need to remove a file that was just marked read-only.
    std::filesystem::permissions(path, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, ec);
    const bool removed = std::filesystem::remove(path, ec);
    if (ec) {
        logWarn(kComponent, "Unable to remove managed file during rollback",
                JsonValue::object().set("path", path.string()).set("detail", ec.message()));
    }
    return removed;
}

}  // namespace trace
