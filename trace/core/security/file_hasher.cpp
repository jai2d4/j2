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

}  // namespace trace
