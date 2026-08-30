#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/security/crypto.h"
#include "core/common/time_utils.h"
#include "core/models/media_metadata.h"

namespace trace {

/// One decoded video frame, converted to packed 24-bit RGB.
///
/// `presentationUs` is the frame's real presentation timestamp, normalised so
/// that the first frame of the stream is time zero. It is the authoritative
/// position for bookmarks, annotations and frame exports.
struct VideoFrameData {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;    ///< width * height * 3 bytes
    Microseconds presentationUs = 0;
    Microseconds durationUs = 0;      ///< 0 when the container did not report one
    /// Only populated when the stream's frame timing is regular enough for a
    /// frame index to mean something. Variable-frame-rate media leaves it empty
    /// rather than showing a number that would be wrong.
    std::optional<std::int64_t> frameNumber;
    bool keyFrame = false;

    bool valid() const { return width > 0 && height > 0 && !rgb.empty(); }
};

/// Timing and format facts about the opened media.
struct DecoderStreamInfo {
    int width = 0;
    int height = 0;
    Microseconds durationUs = 0;
    Microseconds startTimeUs = 0;
    double averageFrameRate = 0.0;
    double nominalFrameRate = 0.0;
    FrameRateMode frameRateMode = FrameRateMode::Unknown;
    std::string codecName;
    std::string codecLongName;
    std::string pixelFormat;
    bool hasAudio = false;
    int videoStreamIndex = -1;
    /// The accelerator that decoded this stream, or empty for software. Recorded
    /// on every derived asset, so a frame exported through a hardware path says
    /// which one produced it.
    std::string hardwareDevice;
};

/// Decodes video for the viewer.
///
/// Seeking is accurate rather than keyframe-only: the demuxer seeks backwards to
/// the preceding keyframe and frames are decoded forward until the requested
/// presentation time is reached, so stepping and bookmarking land on the frame
/// the analyst is actually looking at. Nothing here assumes a constant frame
/// rate: every position comes from a real timestamp.
///
/// Instances are not thread-safe; the playback engine owns one on its worker
/// thread.
class VideoDecoder {
public:
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// Opens a recording for decoding.
    ///
    /// `key` is needed only when the file is an encrypted container; a plain
    /// file ignores it. Passing none for an encrypted file is refused rather
    /// than reported as an unreadable format.
    static Result<std::unique_ptr<VideoDecoder>> open(const std::filesystem::path& file,
                                                      const crypto::SecretKey* key = nullptr);

    /// Opens with a named hardware accelerator, falling back to software when it
    /// cannot be used.
    ///
    /// The fallback is deliberate and silent only in the sense that it does not
    /// fail: `usingHardware()` and `info().hardwareDevice` report what actually
    /// happened, and that is what gets recorded, so nothing downstream can claim
    /// a device that did not decode anything. An accelerator that is missing,
    /// busy, or unsupported for this codec is an ordinary condition on a mixed
    /// fleet of workstations, not an error worth refusing to play a file over.
    ///
    /// Pass an empty `deviceName` to have one chosen, or use `open` for software.
    static Result<std::unique_ptr<VideoDecoder>> openAccelerated(
        const std::filesystem::path& file, const std::string& deviceName = {},
        const crypto::SecretKey* key = nullptr);

    /// True when frames are coming from an accelerator.
    bool usingHardware() const;

    const DecoderStreamInfo& info() const { return info_; }

    /// Next frame in presentation order. Returns ErrorCode::NotFound at the end
    /// of the stream.
    ///
    /// An accurate seek has to decode one frame beyond its target to know it
    /// has passed it; that frame is held back and returned here, so stepping
    /// forward immediately after a seek advances by exactly one frame.
    Result<VideoFrameData> nextFrame();

    /// Frame whose presentation time is the latest at or before `targetUs`.
    Result<VideoFrameData> frameAt(Microseconds targetUs);

    /// Frame immediately before `currentUs` — the backward frame step.
    Result<VideoFrameData> frameBefore(Microseconds currentUs);

    /// Nominal frame duration, used only for jump sizing and UI hints.
    Microseconds estimatedFrameDurationUs() const;

private:
    VideoDecoder() = default;

    static Result<std::unique_ptr<VideoDecoder>> openInternal(const std::filesystem::path& file,
                                                              const crypto::SecretKey* key,
                                                              const std::string& requestedDevice,
                                                              bool allowHardware);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    DecoderStreamInfo info_;
};

}  // namespace trace
