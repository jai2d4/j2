#pragma once

#include <filesystem>
#include <string>

#include "core/common/json.h"
#include "core/common/result.h"
#include "core/security/crypto.h"
#include "core/models/media_metadata.h"

namespace trace {

/// Boundary between the evidence core and whatever reads media containers.
///
/// The core knows nothing about FFmpeg: the media module supplies an
/// implementation, and tests supply a stub. Metadata extraction is allowed to
/// fail without failing an import — an unreadable container is a media problem,
/// not an integrity problem.
class IMetadataExtractor {
public:
    virtual ~IMetadataExtractor() = default;

    /// Name and version of the extractor, recorded with the metadata.
    virtual std::string name() const = 0;
    /// Versions of the underlying libraries, for provenance records.
    virtual JsonValue libraryVersions() const = 0;

    /// `key` is needed only when the stored file is an encrypted container;
    /// a plain file ignores it.
    virtual Result<MediaMetadata> extract(const std::filesystem::path& file,
                                          const std::string& evidenceId,
                                          const crypto::SecretKey* key = nullptr) = 0;
};

}  // namespace trace
