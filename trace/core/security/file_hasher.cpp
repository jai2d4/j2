#include "core/security/file_hasher.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/security/sha256.h"

namespace trace {

Result<std::string> hashFile(const std::filesystem::path& path, const ProgressCallback& progress) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Result<std::string>::failure(ErrorCode::NotFound,
                                            "File not found: " + path.string());
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Result<std::string>::failure(ErrorCode::IoError,
                                            "Unable to determine file size: " + path.string(),
                                            ec.message());
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<std::string>::failure(ErrorCode::IoError,
                                            "Unable to open file for reading: " + path.string(),
                                            std::strerror(errno));
    }

    Sha256 hasher;
    std::vector<char> buffer(kStreamChunkSize);
    HashProgress state{0, static_cast<std::int64_t>(size)};

    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read <= 0) break;
        hasher.update(buffer.data(), static_cast<std::size_t>(read));
        state.bytesProcessed += read;
        if (progress && !progress(state)) {
            return Result<std::string>::failure(ErrorCode::Cancelled,
                                                "Hashing cancelled by operator");
        }
    }
    if (input.bad()) {
        return Result<std::string>::failure(ErrorCode::IoError,
                                            "Read error while hashing: " + path.string(),
                                            std::strerror(errno));
    }
    return Result<std::string>::success(hasher.finalizeHex());
}

Result<std::string> hashStoredEvidence(const std::filesystem::path& path,
                                       const crypto::SecretKey* key,
                                       const ProgressCallback& progress) {
    using ResultType = Result<std::string>;

    // Not "is there a key" but "is this file a container". A workspace can hold
    // both: everything ingested before encryption was switched on is a plain
    // file, and its digest is still the digest of the evidence.
    if (key == nullptr || !crypto::looksEncrypted(path)) {
        return hashFile(path, progress);
    }

    crypto::EncryptedFileReader reader;
    if (auto status = reader.open(path, *key); !status) return ResultType(status.error());

    Sha256 hasher;
    std::vector<std::uint8_t> buffer(kStreamChunkSize);
    HashProgress state{0, static_cast<std::int64_t>(reader.size())};

    std::uint64_t offset = 0;
    while (offset < reader.size()) {
        auto got = reader.read(offset, buffer.data(), buffer.size());
        if (!got) return ResultType(got.error());
        const std::size_t bytes = got.take();
        if (bytes == 0) {
            // The header said there was more. Stopping here would produce a
            // digest of a shorter file and report it as the evidence digest,
            // which is the one thing an integrity check must never do.
            return ResultType::failure(ErrorCode::IntegrityFailure,
                                       "Encrypted evidence ended before its recorded length",
                                       "Read " + std::to_string(offset) + " of " +
                                           std::to_string(reader.size()) + " bytes.");
        }
        hasher.update(reinterpret_cast<const char*>(buffer.data()), bytes);
        offset += bytes;
        state.bytesProcessed = static_cast<std::int64_t>(offset);
        if (progress && !progress(state)) {
            return ResultType::failure(ErrorCode::Cancelled, "Hashing cancelled by operator");
        }
    }
    return ResultType::success(hasher.finalizeHex());
}

}  // namespace trace
