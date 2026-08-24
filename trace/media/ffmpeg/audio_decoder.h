#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/common/time_utils.h"

namespace trace {

/// One decoded block of audio, resampled to the format the caller asked for.
///
/// Samples are interleaved: for stereo, L R L R … `presentationUs` is the real
/// presentation timestamp of the first sample, normalised so the first block of the
/// stream is time zero — the same convention video frames use, so the two line up.
struct AudioBlockData {
    std::vector<std::int16_t> samples;  ///< interleaved, `channels` per sample frame
    int channels = 0;
    int sampleRate = 0;
    Microseconds presentationUs = 0;

    std::size_t sampleFrames() const {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
    Microseconds durationUs() const {
        return sampleRate > 0
                   ? static_cast<Microseconds>(sampleFrames() * 1'000'000ULL / static_cast<unsigned>(sampleRate))
                   : 0;
    }
    bool valid() const { return channels > 0 && sampleRate > 0 && !samples.empty(); }
};

/// What the container actually holds, and what this decoder is producing from it.
///
/// The source fields are reported unchanged so an analyst can see what was recorded;
/// the output fields describe the resampled form the application receives.
struct AudioStreamInfo {
    int streamIndex = -1;
    std::string codecName;
    std::string codecLongName;

    int sourceSampleRate = 0;
    int sourceChannels = 0;
    std::string sourceSampleFormat;
    std::int64_t sourceBitRate = 0;

    int sampleRate = 0;   ///< after resampling
    int channels = 0;
    Microseconds durationUs = 0;
    Microseconds startTimeUs = 0;
};

/// Decodes and resamples the audio track.
///
/// Audio is decoded with FFmpeg and converted to interleaved signed 16-bit PCM through
/// libswresample, because that is the one format every output backend accepts. The
/// conversion is a decode of the original — the managed original is opened read-only
/// and is never rewritten, exactly as with video.
///
/// This class contains no Qt: it produces PCM buffers, and playing them is the UI
/// layer's job. That keeps the audio path testable headlessly, and it is why waveform
/// generation works on a machine with no sound card at all.
///
/// Instances are not thread-safe; a caller owns one on its own thread.
class AudioDecoder {
public:
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    /// Opens the first audio stream. Fails with ErrorCode::NotFound when the file has
    /// none — an absent audio track is a fact about the evidence, not an error to hide.
    static Result<std::unique_ptr<AudioDecoder>> open(const std::filesystem::path& file,
                                                      int targetSampleRate = 48000,
                                                      int targetChannels = 2);

    const AudioStreamInfo& info() const { return info_; }

    /// Next block in presentation order. Returns ErrorCode::NotFound at the end of the
    /// stream, which is how a caller knows it has read everything.
    Result<AudioBlockData> nextBlock();

    /// Moves to `positionUs`. Decoding resumes from the packet at or before it, so the
    /// first block returned afterwards may begin slightly earlier than requested; its
    /// timestamp says where it really starts.
    Status seek(Microseconds positionUs);

private:
    AudioDecoder();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    AudioStreamInfo info_;
};

}  // namespace trace
