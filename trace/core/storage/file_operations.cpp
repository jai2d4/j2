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
                                           const ProgressCallback& progress,
                                           const crypto::SecretKey* key) {
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

    // Exactly one of these is used. The encrypting path needs the plaintext
    // length up front, because the container header carries it and is
    // authenticated — it cannot be patched in after the fact.
    std::ofstream output;
    crypto::EncryptedFileWriter encryptedOutput;
    if (key != nullptr) {
        if (auto status = encryptedOutput.begin(destination, *key, totalBytes); !status) {
            return ResultType(status.error());
        }
    } else {
        output.open(destination, std::ios::binary | std::ios::trunc);
        if (!output) {
            return ResultType::failure(ErrorCode::IoError,
                                       "Unable to create managed copy: " + destination.string(),
                                       std::strerror(errno));
        }
    }
    const auto writeFailed = [&](const char* what) {
        if (output.is_open()) output.close();
        removeManagedFile(destination);
        return ResultType::failure(ErrorCode::IoError, what, std::strerror(errno));
    };

    Sha256 hasher;
    std::vector<char> buffer(kStreamChunkSize);
    CopyOutcome outcome;
    HashProgress state{0, static_cast<std::int64_t>(totalBytes)};

    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read <= 0) break;

        if (key != nullptr) {
            auto written = encryptedOutput.write(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                                 static_cast<std::size_t>(read));
            if (!written) {
                removeManagedFile(destination);
                return ResultType(written.error());
            }
        } else {
            output.write(buffer.data(), read);
            if (!output) return writeFailed("Write failed while copying into managed storage");
        }
        hasher.update(buffer.data(), static_cast<std::size_t>(read));
        outcome.bytesCopied += read;
        state.bytesProcessed = outcome.bytesCopied;

        if (progress && !progress(state)) {
            if (output.is_open()) output.close();
            removeManagedFile(destination);
            return ResultType::failure(ErrorCode::Cancelled, "Import cancelled by operator");
        }
    }

    if (input.bad()) {
        if (output.is_open()) output.close();
        removeManagedFile(destination);
        return ResultType::failure(ErrorCode::IoError, "Read failed while copying source file",
                                   std::strerror(errno));
    }

    if (key != nullptr) {
        // finish() is also where a short write is caught: the header promised a
        // length, and a container that does not reach it cannot be fully read.
        if (auto status = encryptedOutput.finish(); !status) {
            removeManagedFile(destination);
            return ResultType(status.error());
        }
    } else {
        output.flush();
        if (!output) return writeFailed("Unable to flush managed copy to disk");
        output.close();
    }
    outcome.sourceSha256 = hasher.finalizeHex();

    if (key == nullptr) {
        // A container is legitimately larger than its plaintext, so this check
        // belongs to the byte-copy path only; the encrypted path proves the same
        // thing by decrypting the whole file below.
        const auto writtenSize = std::filesystem::file_size(destination, ec);
        if (ec || writtenSize != totalBytes) {
            removeManagedFile(destination);
            return ResultType::failure(ErrorCode::IoError,
                                       "Managed copy is not the same size as the source",
                                       "expected " + std::to_string(totalBytes) + " bytes, wrote " +
                                           std::to_string(ec ? 0 : writtenSize));
        }
    }

    // Independent verification pass over what actually landed on disk. For an
    // encrypted copy this reads back through the container, so it proves both
    // that the bytes are right and that they can be decrypted at all.
    auto destinationHash = hashStoredEvidence(destination, key, progress);
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
