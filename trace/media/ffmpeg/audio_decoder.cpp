#include "media/ffmpeg/audio_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "core/common/logging.h"
#include "media/ffmpeg/ffmpeg_support.h"

namespace trace {
namespace {

constexpr const char* kComponent = "audio";
constexpr AVRational kMicroseconds{1, 1000000};
constexpr AVSampleFormat kOutputFormat = AV_SAMPLE_FMT_S16;

}  // namespace

struct AudioDecoder::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwrContext* resampler = nullptr;
    bool drained = false;
    /// Timestamp of the first block, subtracted so the stream starts at zero.
    Microseconds startOffsetUs = 0;
    /// Where the next block begins when the container gives no timestamp.
    Microseconds nextPositionUs = 0;
    int outputChannels = 0;
    int outputSampleRate = 0;

    ~Impl() {
        if (resampler != nullptr) swr_free(&resampler);
        if (packet != nullptr) av_packet_free(&packet);
        if (frame != nullptr) av_frame_free(&frame);
        if (codec != nullptr) avcodec_free_context(&codec);
        if (format != nullptr) avformat_close_input(&format);
    }
};

AudioDecoder::AudioDecoder() : impl_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;

Result<std::unique_ptr<AudioDecoder>> AudioDecoder::open(const std::filesystem::path& file,
                                                         int targetSampleRate,
                                                         int targetChannels) {
    using ResultType = Result<std::unique_ptr<AudioDecoder>>;
    initialiseFFmpeg();

    if (targetSampleRate <= 0 || targetChannels <= 0 || targetChannels > 8) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "Unusable target audio format requested");
    }

    std::unique_ptr<AudioDecoder> decoder(new AudioDecoder());
    Impl& impl = *decoder->impl_;

    int rc = avformat_open_input(&impl.format, file.string().c_str(), nullptr, nullptr);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not open the media: " + ffmpegErrorString(rc));
    }
    rc = avformat_find_stream_info(impl.format, nullptr);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not read stream information: " + ffmpegErrorString(rc));
    }

    const AVCodec* codec = nullptr;
    const int index = av_find_best_stream(impl.format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (index < 0 || codec == nullptr) {
        return ResultType::failure(ErrorCode::NotFound,
                                   "This item has no audio track");
    }
    impl.stream = impl.format->streams[index];

    impl.codec = avcodec_alloc_context3(codec);
    if (impl.codec == nullptr) {
        return ResultType::failure(ErrorCode::Internal, "Out of memory opening the audio decoder");
    }
    rc = avcodec_parameters_to_context(impl.codec, impl.stream->codecpar);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not configure the audio decoder: " + ffmpegErrorString(rc));
    }
    rc = avcodec_open2(impl.codec, codec, nullptr);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::Unsupported,
                                   "This audio codec could not be opened: " + ffmpegErrorString(rc));
    }

    impl.frame = av_frame_alloc();
    impl.packet = av_packet_alloc();
    if (impl.frame == nullptr || impl.packet == nullptr) {
        return ResultType::failure(ErrorCode::Internal, "Out of memory opening the audio decoder");
    }

    // ------------------------------------------------------------ resampler
    AVChannelLayout outputLayout;
    av_channel_layout_default(&outputLayout, targetChannels);
    rc = swr_alloc_set_opts2(&impl.resampler, &outputLayout, kOutputFormat, targetSampleRate,
                             &impl.codec->ch_layout, impl.codec->sample_fmt,
                             impl.codec->sample_rate, 0, nullptr);
    if (rc < 0 || impl.resampler == nullptr) {
        av_channel_layout_uninit(&outputLayout);
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not set up audio resampling: " + ffmpegErrorString(rc));
    }
    rc = swr_init(impl.resampler);
    av_channel_layout_uninit(&outputLayout);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not initialise audio resampling: " + ffmpegErrorString(rc));
    }

    impl.outputChannels = targetChannels;
    impl.outputSampleRate = targetSampleRate;

    // --------------------------------------------------------------- facts
    AudioStreamInfo& info = decoder->info_;
    info.streamIndex = index;
    info.codecName = codec->name != nullptr ? codec->name : "";
    info.codecLongName = codec->long_name != nullptr ? codec->long_name : "";
    info.sourceSampleRate = impl.codec->sample_rate;
    info.sourceChannels = impl.codec->ch_layout.nb_channels;
    const char* sampleFormat = av_get_sample_fmt_name(impl.codec->sample_fmt);
    info.sourceSampleFormat = sampleFormat != nullptr ? sampleFormat : "";
    info.sourceBitRate = impl.stream->codecpar->bit_rate;
    info.sampleRate = targetSampleRate;
    info.channels = targetChannels;

    if (impl.stream->duration != AV_NOPTS_VALUE) {
        info.durationUs = av_rescale_q(impl.stream->duration, impl.stream->time_base, kMicroseconds);
    } else if (impl.format->duration != AV_NOPTS_VALUE) {
        info.durationUs = impl.format->duration;
    }
    if (impl.stream->start_time != AV_NOPTS_VALUE) {
        info.startTimeUs =
            av_rescale_q(impl.stream->start_time, impl.stream->time_base, kMicroseconds);
        impl.startOffsetUs = info.startTimeUs;
    }

    logInfo(kComponent, "Audio stream opened",
            JsonValue::object()
                .set("file", file.filename().string())
                .set("codec", info.codecName)
                .set("source_sample_rate", static_cast<std::int64_t>(info.sourceSampleRate))
                .set("source_channels", static_cast<std::int64_t>(info.sourceChannels))
                .set("output_sample_rate", static_cast<std::int64_t>(info.sampleRate))
                .set("output_channels", static_cast<std::int64_t>(info.channels)));

    return ResultType::success(std::move(decoder));
}

Result<AudioBlockData> AudioDecoder::nextBlock() {
    using ResultType = Result<AudioBlockData>;
    Impl& impl = *impl_;

    for (;;) {
        int rc = avcodec_receive_frame(impl.codec, impl.frame);

        if (rc == 0) {
            AudioBlockData block;
            block.channels = impl.outputChannels;
            block.sampleRate = impl.outputSampleRate;

            // Timestamp from the container where there is one; otherwise continue
            // from where the previous block ended, so a stream with sparse
            // timestamps still produces a continuous, honest position.
            const std::int64_t pts =
                impl.frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? impl.frame->best_effort_timestamp
                    : impl.frame->pts;
            if (pts != AV_NOPTS_VALUE) {
                const Microseconds absolute =
                    av_rescale_q(pts, impl.stream->time_base, kMicroseconds);
                block.presentationUs = absolute - impl.startOffsetUs;
                if (block.presentationUs < 0) block.presentationUs = 0;
            } else {
                block.presentationUs = impl.nextPositionUs;
            }

            // Resampling can hold samples back, so ask how many will come out.
            const int delay = static_cast<int>(
                swr_get_delay(impl.resampler, impl.codec->sample_rate));
            const int maxOut = static_cast<int>(av_rescale_rnd(
                delay + impl.frame->nb_samples, impl.outputSampleRate, impl.codec->sample_rate,
                AV_ROUND_UP));

            block.samples.resize(static_cast<std::size_t>(maxOut) *
                                 static_cast<std::size_t>(impl.outputChannels));
            auto* output = reinterpret_cast<std::uint8_t*>(block.samples.data());

            const int produced =
                swr_convert(impl.resampler, &output, maxOut,
                            const_cast<const std::uint8_t**>(impl.frame->extended_data),
                            impl.frame->nb_samples);
            av_frame_unref(impl.frame);

            if (produced < 0) {
                return ResultType::failure(ErrorCode::MediaError,
                                           "Audio resampling failed: " + ffmpegErrorString(produced));
            }
            block.samples.resize(static_cast<std::size_t>(produced) *
                                 static_cast<std::size_t>(impl.outputChannels));
            if (produced == 0) continue;  // everything was buffered; read more

            impl.nextPositionUs = block.presentationUs + block.durationUs();
            return ResultType::success(std::move(block));
        }

        if (rc == AVERROR_EOF) {
            return ResultType::failure(ErrorCode::NotFound, "End of the audio stream");
        }
        if (rc != AVERROR(EAGAIN)) {
            return ResultType::failure(ErrorCode::MediaError,
                                       "Audio decoding failed: " + ffmpegErrorString(rc));
        }

        // The decoder wants another packet.
        if (impl.drained) {
            return ResultType::failure(ErrorCode::NotFound, "End of the audio stream");
        }

        bool sent = false;
        while (!sent) {
            rc = av_read_frame(impl.format, impl.packet);
            if (rc == AVERROR_EOF) {
                avcodec_send_packet(impl.codec, nullptr);  // flush
                impl.drained = true;
                sent = true;
                break;
            }
            if (rc < 0) {
                return ResultType::failure(ErrorCode::MediaError,
                                           "Could not read the media: " + ffmpegErrorString(rc));
            }
            if (impl.packet->stream_index != info_.streamIndex) {
                av_packet_unref(impl.packet);
                continue;
            }
            rc = avcodec_send_packet(impl.codec, impl.packet);
            av_packet_unref(impl.packet);
            if (rc < 0 && rc != AVERROR(EAGAIN)) {
                return ResultType::failure(ErrorCode::MediaError,
                                           "Audio decoding failed: " + ffmpegErrorString(rc));
            }
            sent = true;
        }
    }
}

Status AudioDecoder::seek(Microseconds positionUs) {
    Impl& impl = *impl_;
    if (positionUs < 0) positionUs = 0;

    const std::int64_t target =
        av_rescale_q(positionUs + impl.startOffsetUs, kMicroseconds, impl.stream->time_base);
    const int rc = av_seek_frame(impl.format, info_.streamIndex, target, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        return Status::failure(ErrorCode::MediaError,
                               "Could not seek the audio stream: " + ffmpegErrorString(rc));
    }

    avcodec_flush_buffers(impl.codec);
    impl.drained = false;
    impl.nextPositionUs = positionUs;
    return Status::success();
}

}  // namespace trace
