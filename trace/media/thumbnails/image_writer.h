#pragma once

#include <filesystem>
#include <optional>

#include "core/common/result.h"
#include "media/ffmpeg/video_decoder.h"

namespace trace {

/// Writes a decoded frame to a PNG file using libavcodec's PNG encoder.
///
/// PNG is lossless: an exported frame is exactly the decoded pixels, with no
/// second generation of compression loss on top of the evidence.
/// `maxWidth`, when given, scales the image down for preview use — full-size
/// exports pass nothing.
Status writeFramePng(const VideoFrameData& frame, const std::filesystem::path& destination,
                     std::optional<int> maxWidth = std::nullopt);

}  // namespace trace
