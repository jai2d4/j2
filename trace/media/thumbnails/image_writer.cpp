#include "media/thumbnails/image_writer.h"

#include <cstdio>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "core/common/logging.h"
#include "media/ffmpeg/ffmpeg_support.h"

namespace trace {
namespace {

constexpr const char* kComponent = "image";

struct EncoderResources {
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;

    ~EncoderResources() {
        if (scaler != nullptr) sws_freeContext(scaler);
        if (packet != nullptr) av_packet_free(&packet);
        if (frame != nullptr) av_frame_free(&frame);
        if (context != nullptr) avcodec_free_context(&context);
    }
};

}  // namespace

Status writeFramePng(const VideoFrameData& frame, const std::filesystem::path& destination,
                     std::optional<int> maxWidth) {
    initialiseFFmpeg();

    if (!frame.valid()) {
        return Status::failure(ErrorCode::InvalidArgument, "There is no decoded frame to save");
    }

    int targetWidth = frame.width;
    int targetHeight = frame.height;
    if (maxWidth && *maxWidth > 0 && frame.width > *maxWidth) {
        targetWidth = *maxWidth;
        targetHeight = static_cast<int>(static_cast<std::int64_t>(frame.height) * targetWidth /
                                        frame.width);
        if (targetHeight <= 0) targetHeight = 1;
        // PNG encoding requires even-free dimensions only for some formats; RGB24
        // has no such constraint, so any positive size is valid here.
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) {
        return Status::failure(ErrorCode::Unsupported,
                               "This build of FFmpeg cannot write PNG images");
    }

    EncoderResources resources;
    resources.context = avcodec_alloc_context3(codec);
    if (resources.context == nullptr) {
        return Status::failure(ErrorCode::Internal, "Unable to allocate the PNG encoder");
    }
    resources.context->width = targetWidth;
    resources.context->height = targetHeight;
    resources.context->pix_fmt = AV_PIX_FMT_RGB24;
    resources.context->time_base = AVRational{1, 1};

    if (const int rc = avcodec_open2(resources.context, codec, nullptr); rc < 0) {
        return Status::failure(ErrorCode::MediaError, "Unable to start the PNG encoder",
                               ffmpegErrorString(rc));
    }

    resources.frame = av_frame_alloc();
    resources.packet = av_packet_alloc();
    if (resources.frame == nullptr || resources.packet == nullptr) {
        return Status::failure(ErrorCode::Internal, "Unable to allocate image buffers");
    }
    resources.frame->format = AV_PIX_FMT_RGB24;
    resources.frame->width = targetWidth;
    resources.frame->height = targetHeight;
    if (const int rc = av_frame_get_buffer(resources.frame, 32); rc < 0) {
        return Status::failure(ErrorCode::Internal, "Unable to allocate the image frame",
                               ffmpegErrorString(rc));
    }

    const std::uint8_t* sourceData[4] = {frame.rgb.data(), nullptr, nullptr, nullptr};
    const int sourceStride[4] = {frame.width * 3, 0, 0, 0};

    if (targetWidth == frame.width && targetHeight == frame.height) {
        av_image_copy(resources.frame->data, resources.frame->linesize, sourceData, sourceStride,
                      AV_PIX_FMT_RGB24, targetWidth, targetHeight);
    } else {
        resources.scaler =
            sws_getContext(frame.width, frame.height, AV_PIX_FMT_RGB24, targetWidth, targetHeight,
                           AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (resources.scaler == nullptr) {
            return Status::failure(ErrorCode::Internal, "Unable to prepare image scaling");
        }
        sws_scale(resources.scaler, sourceData, sourceStride, 0, frame.height,
                  resources.frame->data, resources.frame->linesize);
    }

    if (const int rc = avcodec_send_frame(resources.context, resources.frame); rc < 0) {
        return Status::failure(ErrorCode::MediaError, "The PNG encoder rejected the frame",
                               ffmpegErrorString(rc));
    }
    avcodec_send_frame(resources.context, nullptr);  // flush

    const int rc = avcodec_receive_packet(resources.context, resources.packet);
    if (rc < 0) {
        return Status::failure(ErrorCode::MediaError, "The PNG encoder produced no image",
                               ffmpegErrorString(rc));
    }

    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);

    std::FILE* file = std::fopen(destination.string().c_str(), "wb");
    if (file == nullptr) {
        av_packet_unref(resources.packet);
        return Status::failure(ErrorCode::IoError,
                               "Unable to create the image file: " + destination.string());
    }
    const std::size_t written =
        std::fwrite(resources.packet->data, 1, static_cast<std::size_t>(resources.packet->size), file);
    const bool flushed = std::fflush(file) == 0;
    std::fclose(file);
    const bool complete = written == static_cast<std::size_t>(resources.packet->size) && flushed;
    av_packet_unref(resources.packet);

    if (!complete) {
        std::filesystem::remove(destination, ec);
        return Status::failure(ErrorCode::IoError, "The image file could not be written completely");
    }

    logDebug(kComponent, "Image written",
             JsonValue::object()
                 .set("file", destination.filename().string())
                 .set("width", static_cast<std::int64_t>(targetWidth))
                 .set("height", static_cast<std::int64_t>(targetHeight)));
    return Status::success();
}

}  // namespace trace
