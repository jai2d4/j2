#include "media/ffmpeg/video_decoder.h"

#include <cmath>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "core/common/logging.h"
#include "media/ffmpeg/encrypted_io.h"
#include "media/ffmpeg/ffmpeg_support.h"
#include "media/ffmpeg/hardware_decode.h"

namespace trace {
namespace {

constexpr const char* kComponent = "decoder";
constexpr AVRational kMicroseconds{1, 1000000};

}  // namespace

struct VideoDecoder::Impl {
    // Declared before `format`: the decrypting IO context must outlive the
    // format context that points at it, and members are destroyed in reverse.
    EncryptedMediaIo io;
    // Likewise before `codec`: the device context is referenced by the codec
    // context, and releasing the device first would leave a dangling reference
    // for avcodec_free_context to walk.
    hwaccel::Session hardware;
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    /// Where a hardware frame is read back into system memory. Reused across
    /// frames rather than allocated per frame, which at 25 frames a second is
    /// the difference between a buffer and a treadmill.
    AVFrame* transferred = nullptr;
    int scalerWidth = 0;
    int scalerHeight = 0;
    AVPixelFormat scalerFormat = AV_PIX_FMT_NONE;
    bool drained = false;
    /// Frame decoded past a seek target, waiting to be handed to nextFrame().
    std::optional<VideoFrameData> lookahead;

    ~Impl() {
        if (scaler != nullptr) sws_freeContext(scaler);
        if (packet != nullptr) av_packet_free(&packet);
        if (transferred != nullptr) av_frame_free(&transferred);
        if (frame != nullptr) av_frame_free(&frame);
        if (codec != nullptr) avcodec_free_context(&codec);
        if (format != nullptr) avformat_close_input(&format);
    }
};

VideoDecoder::~VideoDecoder() = default;

Result<std::unique_ptr<VideoDecoder>> VideoDecoder::open(const std::filesystem::path& file,
                                                         const crypto::SecretKey* key) {
    return openInternal(file, key, {}, /*allowHardware=*/false);
}

Result<std::unique_ptr<VideoDecoder>> VideoDecoder::openAccelerated(
    const std::filesystem::path& file, const std::string& deviceName,
    const crypto::SecretKey* key) {
    return openInternal(file, key, deviceName, /*allowHardware=*/true);
}

bool VideoDecoder::usingHardware() const {
    return impl_ != nullptr && impl_->hardware.active();
}

Result<std::unique_ptr<VideoDecoder>> VideoDecoder::openInternal(
    const std::filesystem::path& file, const crypto::SecretKey* key,
    const std::string& requestedDevice, bool allowHardware) {
    using ResultType = Result<std::unique_ptr<VideoDecoder>>;
    initialiseFFmpeg();

    auto decoder = std::unique_ptr<VideoDecoder>(new VideoDecoder());
    decoder->impl_ = std::make_unique<Impl>();
    Impl& impl = *decoder->impl_;

    std::string url;
    if (auto status = impl.io.prepare(&impl.format, file, key, &url); !status) {
        return ResultType(status.error());
    }
    int rc = avformat_open_input(&impl.format, url.c_str(), nullptr, nullptr);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "This file could not be opened for playback",
                                   ffmpegErrorString(rc));
    }
    rc = avformat_find_stream_info(impl.format, nullptr);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "The media streams could not be read",
                                   ffmpegErrorString(rc));
    }

    const AVCodec* codec = nullptr;
    const int streamIndex =
        av_find_best_stream(impl.format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (streamIndex < 0 || codec == nullptr) {
        return ResultType::failure(ErrorCode::Unsupported,
                                   "No decodable video stream was found in this file",
                                   streamIndex < 0 ? ffmpegErrorString(streamIndex)
                                                   : "no decoder available");
    }
    impl.stream = impl.format->streams[streamIndex];

    impl.codec = avcodec_alloc_context3(codec);
    if (impl.codec == nullptr) {
        return ResultType::failure(ErrorCode::Internal, "Unable to allocate a video decoder");
    }
    rc = avcodec_parameters_to_context(impl.codec, impl.stream->codecpar);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError, "Unable to configure the video decoder",
                                   ffmpegErrorString(rc));
    }
    // Frame-accurate stepping needs deterministic output, so decoding stays
    // single-threaded per frame boundary; throughput work belongs to later
    // phases where GPU decode is introduced.
    impl.codec->thread_count = 1;

    // The accelerator is attached before avcodec_open2 and released if opening
    // fails, so a device that is present but unusable for this file costs one
    // failed open rather than a decoder that half works.
    if (allowHardware) {
        const std::string device =
            requestedDevice.empty() ? hwaccel::preferredDeviceFor(codec) : requestedDevice;
        if (device.empty()) {
            logInfo(kComponent, "No usable hardware decoder for this codec; using software",
                    JsonValue::object().set("codec", avcodec_get_name(impl.stream->codecpar->codec_id)));
        } else if (auto attached = impl.hardware.attach(impl.codec, codec, device); !attached) {
            // An ordinary condition on a mixed fleet, not a failure: the file
            // still plays, and what actually decoded it is recorded below.
            logInfo(kComponent, "Hardware decoding unavailable for this file; using software",
                    JsonValue::object()
                        .set("device", device)
                        .set("detail", attached.error().toString()));
        }
    }

    rc = avcodec_open2(impl.codec, codec, nullptr);
    if (rc < 0) {
        return ResultType::failure(
            ErrorCode::Unsupported,
            std::string("The codec in this file is not supported by this build: ") +
                avcodec_get_name(impl.stream->codecpar->codec_id),
            ffmpegErrorString(rc));
    }

    impl.frame = av_frame_alloc();
    impl.packet = av_packet_alloc();
    impl.transferred = av_frame_alloc();
    if (impl.frame == nullptr || impl.packet == nullptr || impl.transferred == nullptr) {
        return ResultType::failure(ErrorCode::Internal, "Unable to allocate decoding buffers");
    }

    DecoderStreamInfo info;
    info.videoStreamIndex = streamIndex;
    info.width = impl.codec->width;
    info.height = impl.codec->height;
    info.codecName = avcodec_get_name(impl.stream->codecpar->codec_id);
    if (const AVCodecDescriptor* descriptor =
            avcodec_descriptor_get(impl.stream->codecpar->codec_id);
        descriptor != nullptr && descriptor->long_name != nullptr) {
        info.codecLongName = descriptor->long_name;
    }
    if (const char* pixelFormat = av_get_pix_fmt_name(impl.codec->pix_fmt); pixelFormat != nullptr) {
        info.pixelFormat = pixelFormat;
    }
    // What decoded it, not what was asked for. Everything downstream that
    // records provenance reads this field, so a request that fell back to
    // software cannot be reported as hardware.
    info.hardwareDevice = impl.hardware.deviceName();
    if (impl.stream->start_time != AV_NOPTS_VALUE) {
        info.startTimeUs = av_rescale_q(impl.stream->start_time, impl.stream->time_base, kMicroseconds);
    }
    if (impl.format->duration != AV_NOPTS_VALUE && impl.format->duration > 0) {
        info.durationUs = impl.format->duration;
    } else if (impl.stream->duration != AV_NOPTS_VALUE && impl.stream->duration > 0) {
        info.durationUs = av_rescale_q(impl.stream->duration, impl.stream->time_base, kMicroseconds);
    }
    if (impl.stream->avg_frame_rate.num > 0 && impl.stream->avg_frame_rate.den > 0) {
        info.averageFrameRate = av_q2d(impl.stream->avg_frame_rate);
    }
    if (impl.stream->r_frame_rate.num > 0 && impl.stream->r_frame_rate.den > 0) {
        info.nominalFrameRate = av_q2d(impl.stream->r_frame_rate);
    }
    if (info.averageFrameRate > 0.0 && info.nominalFrameRate > 0.0) {
        const double largest = std::max(info.averageFrameRate, info.nominalFrameRate);
        const double difference = std::fabs(info.averageFrameRate - info.nominalFrameRate) / largest;
        info.frameRateMode =
            difference > 0.01 ? FrameRateMode::Variable : FrameRateMode::ConstantSuspected;
    }
    info.hasAudio = av_find_best_stream(impl.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
    decoder->info_ = info;

    logInfo(kComponent, "Media opened for playback",
            JsonValue::object()
                .set("file", file.filename().string())
                .set("codec", info.codecName)
                .set("width", static_cast<std::int64_t>(info.width))
                .set("height", static_cast<std::int64_t>(info.height))
                .set("duration_us", info.durationUs)
                .set("frame_rate_mode", toString(info.frameRateMode)));

    return ResultType::success(std::move(decoder));
}

Microseconds VideoDecoder::estimatedFrameDurationUs() const {
    const double rate = info_.averageFrameRate > 0.0 ? info_.averageFrameRate : info_.nominalFrameRate;
    if (rate <= 0.0) return 40000;  // 25 fps fallback for jump sizing only
    return static_cast<Microseconds>(std::llround(1'000'000.0 / rate));
}

Result<VideoFrameData> VideoDecoder::nextFrame() {
    using ResultType = Result<VideoFrameData>;
    Impl& impl = *impl_;

    if (impl.lookahead.has_value()) {
        VideoFrameData frame = std::move(*impl.lookahead);
        impl.lookahead.reset();
        return ResultType::success(std::move(frame));
    }

    for (;;) {
        const int receive = avcodec_receive_frame(impl.codec, impl.frame);
        if (receive == 0) {
            // A hardware frame lives in device memory and has no pixels to
            // scale. This is where it comes back, or where the attempt fails
            // loudly rather than producing the previous frame again.
            AVFrame* usable = impl.frame;
            if (auto moved = impl.hardware.transfer(impl.frame, impl.transferred, &usable);
                !moved) {
                av_frame_unref(impl.frame);
                return ResultType(moved.error());
            }

            VideoFrameData data;
            data.width = usable->width;
            data.height = usable->height;
            data.keyFrame = usable->flags & AV_FRAME_FLAG_KEY;

            std::int64_t timestamp = usable->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE) timestamp = usable->pts;
            if (timestamp == AV_NOPTS_VALUE) {
                data.presentationUs = 0;
            } else {
                data.presentationUs =
                    av_rescale_q(timestamp, impl.stream->time_base, kMicroseconds) - info_.startTimeUs;
                if (data.presentationUs < 0) data.presentationUs = 0;
            }
            if (usable->duration > 0) {
                data.durationUs =
                    av_rescale_q(usable->duration, impl.stream->time_base, kMicroseconds);
            }
            // A frame index is only meaningful when the timing is regular.
            if (info_.frameRateMode == FrameRateMode::ConstantSuspected &&
                info_.averageFrameRate > 0.0) {
                data.frameNumber = static_cast<std::int64_t>(std::llround(
                    static_cast<double>(data.presentationUs) * info_.averageFrameRate / 1'000'000.0));
            }

            const auto sourceFormat = static_cast<AVPixelFormat>(usable->format);
            if (impl.scaler == nullptr || impl.scalerWidth != usable->width ||
                impl.scalerHeight != usable->height || impl.scalerFormat != sourceFormat) {
                if (impl.scaler != nullptr) sws_freeContext(impl.scaler);
                impl.scaler = sws_getContext(usable->width, usable->height, sourceFormat,
                                             usable->width, usable->height, AV_PIX_FMT_RGB24,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr);
                impl.scalerWidth = usable->width;
                impl.scalerHeight = usable->height;
                impl.scalerFormat = sourceFormat;
            }
            if (impl.scaler == nullptr) {
                return ResultType::failure(ErrorCode::MediaError,
                                           "Unable to convert the decoded frame for display");
            }

            const int stride = usable->width * 3;
            data.rgb.resize(static_cast<std::size_t>(stride) * usable->height);
            std::uint8_t* destination[4] = {data.rgb.data(), nullptr, nullptr, nullptr};
            int destinationStride[4] = {stride, 0, 0, 0};
            sws_scale(impl.scaler, usable->data, usable->linesize, 0, usable->height,
                      destination, destinationStride);

            av_frame_unref(impl.frame);
            if (usable != impl.frame) av_frame_unref(impl.transferred);
            return ResultType::success(std::move(data));
        }
        if (receive == AVERROR_EOF) {
            return ResultType::failure(ErrorCode::NotFound, "End of media");
        }
        if (receive != AVERROR(EAGAIN)) {
            return ResultType::failure(ErrorCode::MediaError, "Frame decoding failed",
                                       ffmpegErrorString(receive));
        }

        // Decoder wants more input.
        if (impl.drained) {
            return ResultType::failure(ErrorCode::NotFound, "End of media");
        }
        const int read = av_read_frame(impl.format, impl.packet);
        if (read == AVERROR_EOF) {
            avcodec_send_packet(impl.codec, nullptr);  // flush
            impl.drained = true;
            continue;
        }
        if (read < 0) {
            return ResultType::failure(ErrorCode::MediaError, "Unable to read from the media file",
                                       ffmpegErrorString(read));
        }
        if (impl.packet->stream_index != info_.videoStreamIndex) {
            av_packet_unref(impl.packet);
            continue;
        }
        const int send = avcodec_send_packet(impl.codec, impl.packet);
        av_packet_unref(impl.packet);
        if (send < 0 && send != AVERROR(EAGAIN)) {
            return ResultType::failure(ErrorCode::MediaError, "The decoder rejected a packet",
                                       ffmpegErrorString(send));
        }
    }
}

Result<VideoFrameData> VideoDecoder::frameAt(Microseconds targetUs) {
    using ResultType = Result<VideoFrameData>;
    Impl& impl = *impl_;

    if (targetUs < 0) targetUs = 0;
    const std::int64_t seekTarget =
        av_rescale_q(targetUs + info_.startTimeUs, kMicroseconds, impl.stream->time_base);

    const int rc = av_seek_frame(impl.format, info_.videoStreamIndex, seekTarget, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError, "Unable to seek in this media file",
                                   ffmpegErrorString(rc));
    }
    avcodec_flush_buffers(impl.codec);
    impl.drained = false;
    impl.lookahead.reset();

    // Decode forward from the keyframe, keeping the latest frame that starts at
    // or before the requested time. This is what makes seeking land on the
    // frame the analyst asked for even when keyframes are seconds apart.
    std::optional<VideoFrameData> candidate;
    for (;;) {
        auto frame = nextFrame();
        if (!frame) {
            if (frame.error().code() == ErrorCode::NotFound && candidate) {
                return ResultType::success(std::move(*candidate));
            }
            return ResultType(frame.error());
        }
        VideoFrameData data = frame.take();
        if (data.presentationUs <= targetUs) {
            candidate = std::move(data);
            continue;
        }
        // First frame after the target. Keep it for the next sequential read so
        // no frame is skipped, and return the frame the caller asked for — or
        // this one if the seek landed past the request, which can happen at the
        // very start of a stream.
        if (candidate) {
            impl.lookahead = std::move(data);
            return ResultType::success(std::move(*candidate));
        }
        return ResultType::success(std::move(data));
    }
}

Result<VideoFrameData> VideoDecoder::frameBefore(Microseconds currentUs) {
    if (currentUs <= 0) return frameAt(0);
    return frameAt(currentUs - 1);
}

}  // namespace trace
