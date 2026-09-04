#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "core/common/result.h"
#include "core/security/file_hasher.h"

namespace trace {

struct CopyOutcome {
    std::int64_t bytesCopied = 0;
    std::string sourceSha256;       ///< computed while reading the source
    std::string destinationSha256;  ///< computed by re-reading what was written
};

/// Copies a file into managed storage and proves the copy is byte-identical.
///
/// The source is opened read-only and never modified. The destination is
/// hashed by an independent second pass, so a truncated or silently corrupted
/// write is caught at ingestion rather than at the first integrity check
/// months later. Refuses to overwrite an existing destination.
Result<CopyOutcome> copyIntoManagedStorage(const std::filesystem::path& source,
                                           const std::filesystem::path& destination,
                                           const ProgressCallback& progress = {});

/// Best-effort last-modified time of a file as an ISO-8601 UTC string.
std::optional<std::string> fileModifiedTimeIso8601(const std::filesystem::path& path);

/// Marks a managed original read-only at the filesystem level. Advisory: it
/// stops accidental edits, not a determined operator, and its failure is not
/// fatal to ingestion.
bool setReadOnly(const std::filesystem::path& path);

/// Removes a file that TRACE itself created inside managed storage. Used only
/// for rollback of a failed ingestion.
bool removeManagedFile(const std::filesystem::path& path);

}  // namespace trace
