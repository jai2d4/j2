#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trace {

/// How the frame timing of a video stream behaves. TRACE reports this instead
/// of assuming constant frame rate, because timestamps derived from an assumed
/// CFR grid are wrong for most body-camera and phone footage.
enum class FrameRateMode { Unknown, ConstantSuspected, Variable };

const char* toString(FrameRateMode mode);
const char* toDisplayString(FrameRateMode mode);
FrameRateMode frameRateModeFromString(const std::string& text,
                                      FrameRateMode fallback = FrameRateMode::Unknown);

enum class StreamType { Video, Audio, Subtitle, Data, Attachment, Unknown };
const char* toString(StreamType type);
StreamType streamTypeFromString(const std::string& text);

/// One elementary stream inside the container.
struct MediaStreamInfo {
    std::string id;
    std::string evidenceId;
    int index = 0;
    StreamType type = StreamType::Unknown;
    std::optional<std::string> codecName;
    std::optional<std::string> codecLongName;
    std::optional<std::string> timeBase;
    std::optional<std::int64_t> durationUs;
    std::optional<std::int64_t> bitRate;
    std::optional<std::int64_t> width;
    std::optional<std::int64_t> height;
    std::optional<std::string> pixelFormat;
    std::optional<double> frameRate;
    std::optional<std::int64_t> sampleRate;
    std::optional<std::int64_t> channels;
    std::optional<std::string> channelLayout;
    std::optional<std::string> language;
    std::string metadataJson = "{}";
};

enum class MetadataExtractionStatus { Unknown, Ok, Partial, Failed };
const char* toString(MetadataExtractionStatus status);
const char* toDisplayString(MetadataExtractionStatus status);
MetadataExtractionStatus metadataExtractionStatusFromString(
    const std::string& text, MetadataExtractionStatus fallback = MetadataExtractionStatus::Unknown);

/// Technical metadata for one evidence item. Every field is optional: TRACE
/// displays "Not available" rather than inventing a value the container did not
/// contain.
struct MediaMetadata {
    std::string evidenceId;
    std::optional<std::string> containerFormat;
    std::optional<std::string> formatLongName;
    std::optional<std::int64_t> durationUs;
    std::optional<std::int64_t> bitRate;

    std::optional<std::string> videoCodec;
    std::optional<std::string> videoCodecLongName;
    std::optional<std::string> videoProfile;
    std::optional<std::int64_t> width;
    std::optional<std::int64_t> height;
    std::optional<std::string> pixelFormat;
    std::optional<std::string> displayAspect;
    std::optional<double> averageFrameRate;
    std::optional<double> nominalFrameRate;
    FrameRateMode frameRateMode = FrameRateMode::Unknown;
    std::optional<std::string> videoTimeBase;
    std::optional<std::int64_t> videoBitRate;
    std::optional<std::int64_t> frameCount;

    std::optional<std::string> audioCodec;
    std::optional<std::string> audioCodecLongName;
    std::optional<std::int64_t> audioChannels;
    std::optional<std::string> audioChannelLayout;
    std::optional<std::int64_t> audioSampleRate;
    std::optional<std::string> audioSampleFormat;
    std::optional<std::int64_t> audioBitRate;

    std::int64_t videoStreamCount = 0;
    std::int64_t audioStreamCount = 0;
    std::int64_t subtitleStreamCount = 0;

    std::optional<std::string> creationTime;
    std::optional<std::string> gpsLocation;
    std::optional<std::string> make;
    std::optional<std::string> model;

    std::string rawMetadataJson = "{}";
    MetadataExtractionStatus extractionStatus = MetadataExtractionStatus::Unknown;
    std::optional<std::string> extractionError;
    std::string extractor;
    std::string extractedAt;

    std::vector<MediaStreamInfo> streams;
};

}  // namespace trace
