#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/media_metadata.h"

namespace trace {

class MetadataRepository {
public:
    explicit MetadataRepository(std::shared_ptr<Database> database);

    /// Writes the metadata row and replaces the stream rows for the evidence
    /// item, atomically.
    Status save(const MediaMetadata& metadata);

    Result<std::optional<MediaMetadata>> findByEvidenceId(const std::string& evidenceId);
    Result<std::vector<MediaStreamInfo>> listStreams(const std::string& evidenceId);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
