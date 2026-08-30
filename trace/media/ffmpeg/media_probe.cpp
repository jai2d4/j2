#include "media/ffmpeg/media_probe.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

#include "core/common/logging.h"
#include "core/common/string_utils.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "media/ffmpeg/encrypted_io.h"
#include "media/ffmpeg/ffmpeg_support.h"
#include "trace/trace_version.h"

namespace trace {
namespace {

constexpr const char* kComponent = "probe";

std::optional<std::string> optionalText(const char* text) {
    if (text == nullptr || *text == '\0') return std::nullopt;
    return std::string(text);
}

std::optional<std::int64_t> optionalPositive(std::int64_t value) {
    if (value <= 0) return std::nullopt;
    return value;
}

std::optional<double> rationalToDouble(AVRational rational) {
    if (rational.den == 0 || rational.num == 0) return std::nullopt;
    return av_q2d(rational);
}

std::string rationalToString(AVRational rational) {
    return std::to_string(rational.num) + "/" + std::to_string(rational.den);
}

JsonValue dictionaryToJson(const AVDictionary* dictionary) {
    JsonValue object = JsonValue::object();
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_iterate(dictionary, entry)) != nullptr) {
        object.set(entry->key, entry->value != nullptr ? entry->value : "");
    }
    return object;
}

std::optional<std::string> lookup(const AVDictionary* dictionary,
                                  std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (const AVDictionaryEntry* entry = av_dict_get(dictionary, key, nullptr, 0);
            entry != nullptr && entry->value != nullptr && *entry->value != '\0') {
            return std::string(entry->value);
        }
    }
    return std::nullopt;
}

StreamType toStreamType(AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:      return StreamType::Video;
        case AVMEDIA_TYPE_AUDIO:      return StreamType::Audio;
        case AVMEDIA_TYPE_SUBTITLE:   return StreamType::Subtitle;
        case AVMEDIA_TYPE_DATA:       return StreamType::Data;
        case AVMEDIA_TYPE_ATTACHMENT: return StreamType::Attachment;
        default:                      return StreamType::Unknown;
    }
}

/// Compares the average and nominal (r_frame_rate) rates a container declares.
/// A meaningful difference is the standard signal that timestamps are variable;
/// TRACE reports that rather than silently treating the media as CFR.
FrameRateMode determineFrameRateMode(const AVStream& stream) {
    const auto average = rationalToDouble(stream.avg_frame_rate);
    const auto nominal = rationalToDouble(stream.r_frame_rate);
    if (!average || !nominal) return FrameRateMode::Unknown;
    const double largest = std::max(*average, *nominal);
    if (largest <= 0.0) return FrameRateMode::Unknown;
    const double difference = std::fabs(*average - *nominal) / largest;
    return difference > 0.01 ? FrameRateMode::Variable : FrameRateMode::ConstantSuspected;
}

std::optional<std::string> channelLayoutName(const AVChannelLayout& layout) {
    char buffer[128] = {};
    if (av_channel_layout_describe(&layout, buffer, sizeof(buffer)) < 0) return std::nullopt;
    return optionalText(buffer);
}

}  // namespace

FFmpegMetadataExtractor::FFmpegMetadataExtractor() { initialiseFFmpeg(); }

std::string FFmpegMetadataExtractor::name() const {
    return std::string("TRACE probe ") + kApplicationVersion + " (" + ffmpegIdentifier() + ")";
}

JsonValue FFmpegMetadataExtractor::libraryVersions() const { return ffmpegLibraryVersions(); }

Result<MediaMetadata> FFmpegMetadataExtractor::extract(const std::filesystem::path& file,
                                                       const std::string& evidenceId,
                                                       const crypto::SecretKey* key) {
    using ResultType = Result<MediaMetadata>;
    initialiseFFmpeg();

    MediaMetadata metadata;
    metadata.evidenceId = evidenceId;
    metadata.extractor = name();
    metadata.extractedAt = nowIso8601Utc();

    // Declared before the context so it is destroyed after it: the decrypting
    // IO must outlive the format context that reads through it.
    EncryptedMediaIo io;
    AVFormatContext* context = nullptr;
    std::string url;
    if (auto status = io.prepare(&context, file, key, &url); !status) {
        return ResultType(status.error());
    }
    const int openResult = avformat_open_input(&context, url.c_str(), nullptr, nullptr);
    if (openResult < 0) {
        const std::string detail = ffmpegErrorString(openResult);
        if (context != nullptr) avformat_close_input(&context);
        return ResultType::failure(
            ErrorCode::MediaError,
            "The file could not be opened as media. The stored evidence is unchanged.", detail);
    }

    const int infoResult = avformat_find_stream_info(context, nullptr);
    if (infoResult < 0) {
        // Partial information is still worth keeping: the container opened.
        logWarn(kComponent, "Stream information incomplete",
                JsonValue::object()
                    .set("file", file.filename().string())
                    .set("detail", ffmpegErrorString(infoResult)));
        metadata.extractionStatus = MetadataExtractionStatus::Partial;
        metadata.extractionError = ffmpegErrorString(infoResult);
    } else {
        metadata.extractionStatus = MetadataExtractionStatus::Ok;
    }

    if (context->iformat != nullptr) {
        metadata.containerFormat = optionalText(context->iformat->name);
        metadata.formatLongName = optionalText(context->iformat->long_name);
    }
    if (context->duration != AV_NOPTS_VALUE && context->duration > 0) {
        metadata.durationUs = static_cast<std::int64_t>(context->duration);
    }
    metadata.bitRate = optionalPositive(context->bit_rate);

    const AVDictionary* formatMetadata = context->metadata;
    metadata.creationTime = lookup(formatMetadata, {"creation_time", "date", "DATE"});
    metadata.gpsLocation = lookup(formatMetadata, {"location", "com.apple.quicktime.location.ISO6709",
                                                   "GPS", "location-eng"});
    metadata.make = lookup(formatMetadata, {"make", "com.apple.quicktime.make", "Make"});
    metadata.model = lookup(formatMetadata, {"model", "com.apple.quicktime.model", "Model"});

    JsonValue raw = JsonValue::object();
    raw.set("probe", JsonValue::object()
                         .set("extractor", metadata.extractor)
                         .set("extracted_at", metadata.extractedAt)
                         .set("libraries", ffmpegLibraryVersions()));
    JsonValue format = JsonValue::object();
    if (metadata.containerFormat) format.set("format_name", *metadata.containerFormat);
    if (metadata.formatLongName) format.set("format_long_name", *metadata.formatLongName);
    if (metadata.durationUs) format.set("duration_us", *metadata.durationUs);
    if (metadata.bitRate) format.set("bit_rate", *metadata.bitRate);
    format.set("nb_streams", static_cast<std::int64_t>(context->nb_streams));
    format.set("metadata", dictionaryToJson(formatMetadata));
    raw.set("format", format);

    JsonValue rawStreams = JsonValue::array();
    bool primaryVideoSeen = false;
    bool primaryAudioSeen = false;

    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        const AVStream* stream = context->streams[index];
        if (stream == nullptr || stream->codecpar == nullptr) continue;
        const AVCodecParameters& parameters = *stream->codecpar;

        MediaStreamInfo info;
        info.id = generateUuid();
        info.evidenceId = evidenceId;
        info.index = static_cast<int>(index);
        info.type = toStreamType(parameters.codec_type);
        info.codecName = optionalText(avcodec_get_name(parameters.codec_id));
        if (const AVCodecDescriptor* descriptor = avcodec_descriptor_get(parameters.codec_id);
            descriptor != nullptr) {
            info.codecLongName = optionalText(descriptor->long_name);
        }
        info.timeBase = rationalToString(stream->time_base);
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            info.durationUs = static_cast<std::int64_t>(
                av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000000}));
        }
        info.bitRate = optionalPositive(parameters.bit_rate);
        info.language = lookup(stream->metadata, {"language"});

        JsonValue rawStream = JsonValue::object();
        rawStream.set("index", info.index).set("type", toString(info.type));
        if (info.codecName) rawStream.set("codec_name", *info.codecName);
        if (info.codecLongName) rawStream.set("codec_long_name", *info.codecLongName);
        rawStream.set("time_base", *info.timeBase);
        if (info.durationUs) rawStream.set("duration_us", *info.durationUs);
        if (info.bitRate) rawStream.set("bit_rate", *info.bitRate);

        switch (info.type) {
            case StreamType::Video: {
                ++metadata.videoStreamCount;
                info.width = optionalPositive(parameters.width);
                info.height = optionalPositive(parameters.height);
                info.pixelFormat =
                    optionalText(av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters.format)));
                info.frameRate = rationalToDouble(stream->avg_frame_rate);

                rawStream.set("width", static_cast<std::int64_t>(parameters.width))
                    .set("height", static_cast<std::int64_t>(parameters.height))
                    .set("avg_frame_rate", rationalToString(stream->avg_frame_rate))
                    .set("r_frame_rate", rationalToString(stream->r_frame_rate))
                    .set("nb_frames", static_cast<std::int64_t>(stream->nb_frames));
                if (info.pixelFormat) rawStream.set("pix_fmt", *info.pixelFormat);

                if (!primaryVideoSeen) {
                    primaryVideoSeen = true;
                    metadata.videoCodec = info.codecName;
                    metadata.videoCodecLongName = info.codecLongName;
                    metadata.width = info.width;
                    metadata.height = info.height;
                    metadata.pixelFormat = info.pixelFormat;
                    metadata.averageFrameRate = rationalToDouble(stream->avg_frame_rate);
                    metadata.nominalFrameRate = rationalToDouble(stream->r_frame_rate);
                    metadata.frameRateMode = determineFrameRateMode(*stream);
                    metadata.videoTimeBase = info.timeBase;
                    metadata.videoBitRate = info.bitRate;
                    metadata.frameCount = optionalPositive(stream->nb_frames);
                    if (const AVCodecDescriptor* descriptor =
                            avcodec_descriptor_get(parameters.codec_id);
                        descriptor != nullptr) {
                        metadata.videoProfile =
                            optionalText(avcodec_profile_name(parameters.codec_id, parameters.profile));
                    }
                    if (parameters.width > 0 && parameters.height > 0) {
                        AVRational display{parameters.width, parameters.height};
                        if (parameters.sample_aspect_ratio.num > 0 &&
                            parameters.sample_aspect_ratio.den > 0) {
                            av_reduce(&display.num, &display.den,
                                      static_cast<std::int64_t>(parameters.width) *
                                          parameters.sample_aspect_ratio.num,
                                      static_cast<std::int64_t>(parameters.height) *
                                          parameters.sample_aspect_ratio.den,
                                      1024 * 1024);
                        } else {
                            av_reduce(&display.num, &display.den, parameters.width,
                                      parameters.height, 1024 * 1024);
                        }
                        metadata.displayAspect =
                            std::to_string(display.num) + ":" + std::to_string(display.den);
                    }
                }
                break;
            }
            case StreamType::Audio: {
                ++metadata.audioStreamCount;
                info.sampleRate = optionalPositive(parameters.sample_rate);
                info.channels = optionalPositive(parameters.ch_layout.nb_channels);
                info.channelLayout = channelLayoutName(parameters.ch_layout);

                rawStream.set("sample_rate", static_cast<std::int64_t>(parameters.sample_rate))
                    .set("channels", static_cast<std::int64_t>(parameters.ch_layout.nb_channels));
                if (info.channelLayout) rawStream.set("channel_layout", *info.channelLayout);

                if (!primaryAudioSeen) {
                    primaryAudioSeen = true;
                    metadata.audioCodec = info.codecName;
                    metadata.audioCodecLongName = info.codecLongName;
                    metadata.audioChannels = info.channels;
                    metadata.audioChannelLayout = info.channelLayout;
                    metadata.audioSampleRate = info.sampleRate;
                    metadata.audioSampleFormat =
                        optionalText(av_get_sample_fmt_name(static_cast<AVSampleFormat>(parameters.format)));
                    metadata.audioBitRate = info.bitRate;
                }
                break;
            }
            case StreamType::Subtitle:
                ++metadata.subtitleStreamCount;
                break;
            default:
                break;
        }

        info.metadataJson = dictionaryToJson(stream->metadata).dump();
        rawStream.set("metadata", dictionaryToJson(stream->metadata));
        rawStreams.push(rawStream);
        metadata.streams.push_back(std::move(info));
    }

    raw.set("streams", rawStreams);
    metadata.rawMetadataJson = raw.dump(2);

    if (metadata.videoStreamCount == 0 && metadata.audioStreamCount == 0 &&
        metadata.extractionStatus == MetadataExtractionStatus::Ok) {
        metadata.extractionStatus = MetadataExtractionStatus::Partial;
        metadata.extractionError = "The container reported no audio or video streams";
    }

    avformat_close_input(&context);

    logInfo(kComponent, "Metadata extracted",
            JsonValue::object()
                .set("file", file.filename().string())
                .set("container", metadata.containerFormat.value_or("unknown"))
                .set("video_streams", metadata.videoStreamCount)
                .set("audio_streams", metadata.audioStreamCount));

    return ResultType::success(std::move(metadata));
}

}  // namespace trace
