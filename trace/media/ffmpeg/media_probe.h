#pragma once

#include <filesystem>
#include <string>

#include "core/common/json.h"
#include "core/common/result.h"
#include "core/models/media_metadata.h"
#include "core/services/metadata_extractor.h"

namespace trace {

/// Reads real container and stream metadata with libavformat.
///
/// Nothing is inferred: a field the container did not report stays empty and is
/// displayed as "Not available". Frame-rate mode is reported as
/// constant-suspected or variable from what the streams actually declare, never
/// assumed.
class FFmpegMetadataExtractor : public IMetadataExtractor {
public:
    FFmpegMetadataExtractor();

    std::string name() const override;
    JsonValue libraryVersions() const override;
    Result<MediaMetadata> extract(const std::filesystem::path& file,
                                  const std::string& evidenceId) override;
};

}  // namespace trace
