#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "core/common/result.h"

namespace trace {

struct HashProgress {
    std::int64_t bytesProcessed = 0;
    std::int64_t totalBytes = 0;
};

/// Return false from a progress callback to cancel the operation. Cancellation
/// is reported as ErrorCode::Cancelled — never as success.
using ProgressCallback = std::function<bool(const HashProgress&)>;

/// Streams `path` through SHA-256 in fixed-size chunks. The file is opened
/// read-only; nothing is written, so this is safe to run against originals.
Result<std::string> hashFile(const std::filesystem::path& path,
                             const ProgressCallback& progress = {});

/// Chunk size used for hashing and copying (1 MiB).
inline constexpr std::size_t kStreamChunkSize = 1024 * 1024;

}  // namespace trace
