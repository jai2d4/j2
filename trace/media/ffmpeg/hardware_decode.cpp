#include "media/ffmpeg/hardware_decode.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include "core/common/logging.h"
#include "media/ffmpeg/ffmpeg_support.h"
#include "media/ffmpeg/video_decoder.h"

namespace trace::hwaccel {
namespace {

constexpr const char* kComponent = "hwaccel";

std::string displayNameFor(const std::string& name) {
    if (name == "cuda") return "NVIDIA CUDA / NVDEC";
    if (name == "vaapi") return "VA-API (Intel / AMD on Linux)";
    if (name == "vdpau") return "VDPAU";
    if (name == "qsv") return "Intel Quick Sync";
    if (name == "d3d11va") return "Direct3D 11 (Windows)";
    if (name == "dxva2") return "DirectX Video Acceleration 2 (Windows)";
    if (name == "videotoolbox") return "VideoToolbox (macOS)";
    if (name == "vulkan") return "Vulkan";
    if (name == "drm") return "DRM";
    if (name == "opencl") return "OpenCL";
    return name;
}

/// Rough order of preference per platform. Not a performance claim — nothing
/// here has been measured — but a correctness one: the accelerator a platform
/// ships as its own is the one most likely to have a working driver and the
/// codec support FFmpeg expects.
int preferenceRank(const std::string& name) {
#if defined(_WIN32)
    if (name == "d3d11va") return 0;
    if (name == "cuda") return 1;
    if (name == "qsv") return 2;
    if (name == "dxva2") return 3;
#elif defined(__APPLE__)
    if (name == "videotoolbox") return 0;
#else
    if (name == "vaapi") return 0;
    if (name == "cuda") return 1;
    if (name == "qsv") return 2;
    if (name == "vdpau") return 3;
#endif
    return 100;
}

/// Probes every accelerator FFmpeg was built with by opening one.
///
/// Opening is the only honest test. An FFmpeg build almost always contains
/// every hardware type it can compile, so "is it in the list" answers a question
/// about the build rather than about the machine, and a settings dialog driven
/// by that would offer an operator a device that does not exist.
std::vector<Device> probeDevices() {
    initialiseFFmpeg();

    std::vector<Device> found;
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        const char* name = av_hwdevice_get_type_name(type);
        if (name == nullptr) continue;

        Device device;
        device.name = name;
        device.displayName = displayNameFor(device.name);

        AVBufferRef* context = nullptr;
        const int rc = av_hwdevice_ctx_create(&context, type, nullptr, nullptr, 0);
        if (rc >= 0 && context != nullptr) {
            device.available = true;
            av_buffer_unref(&context);
        } else {
            device.available = false;
            device.unavailableReason = ffmpegErrorString(rc);
        }
        found.push_back(std::move(device));
    }

    std::stable_sort(found.begin(), found.end(), [](const Device& a, const Device& b) {
        if (a.available != b.available) return a.available;
        return preferenceRank(a.name) < preferenceRank(b.name);
    });

    // Logged once, because "which accelerators does this workstation have" is
    // the first question anyone asks when playback is slow, and the answer
    // should be in the log rather than reconstructed.
    JsonValue summary = JsonValue::array();
    for (const Device& device : found) {
        summary.push(JsonValue::object()
                         .set("name", device.name)
                         .set("available", device.available));
    }
    logInfo(kComponent, "Hardware decoders probed",
            JsonValue::object().set("devices", summary));
    return found;
}

/// The hardware pixel format a codec expects for a device type, or NONE.
AVPixelFormat hardwarePixelFormat(const AVCodec* decoder, AVHWDeviceType type) {
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, index);
        if (config == nullptr) return AV_PIX_FMT_NONE;
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
            config->device_type == type) {
            return config->pix_fmt;
        }
    }
}

/// Chosen at open time and read by FFmpeg's negotiation callback. A codec
/// context carries one opaque pointer, and the decoder already uses it for
/// nothing, so the format travels there rather than in a global.
struct Negotiation {
    AVPixelFormat hardware = AV_PIX_FMT_NONE;
};

AVPixelFormat chooseFormat(AVCodecContext* codec, const AVPixelFormat* formats) {
    const auto* negotiation = static_cast<const Negotiation*>(codec->opaque);
    if (negotiation != nullptr) {
        for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == negotiation->hardware) return *format;
        }
    }
    // FFmpeg's rule: returning a format not in the list aborts the decode. The
    // first entry is always a valid software format, so falling back to it keeps
    // the file playing rather than failing because an accelerator changed its
    // mind after the device opened.
    logInfo(kComponent, "Hardware pixel format was not offered; decoding in software");
    return formats[0];
}

}  // namespace

const std::vector<Device>& devices() {
    static const std::vector<Device> probed = probeDevices();
    return probed;
}

std::vector<Device> availableDevices() {
    std::vector<Device> usable;
    for (const Device& device : devices()) {
        if (device.available) usable.push_back(device);
    }
    return usable;
}

bool anyAvailable() { return !availableDevices().empty(); }

std::string preferredDeviceFor(const AVCodec* decoder) {
    if (decoder == nullptr) return {};
    for (const Device& device : devices()) {
        if (!device.available) continue;
        const AVHWDeviceType type = av_hwdevice_find_type_by_name(device.name.c_str());
        if (type == AV_HWDEVICE_TYPE_NONE) continue;
        // Available *and* supported by this particular codec: a machine can have
        // a working VA-API device and no VA-API support for the codec in hand.
        if (hardwarePixelFormat(decoder, type) != AV_PIX_FMT_NONE) return device.name;
    }
    return {};
}

struct Session::Impl {
    AVBufferRef* deviceContext = nullptr;
    std::string deviceName;
    Negotiation negotiation;
    AVCodecContext* codec = nullptr;

    ~Impl() {
        if (deviceContext != nullptr) av_buffer_unref(&deviceContext);
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;

bool Session::active() const { return impl_->deviceContext != nullptr; }
const std::string& Session::deviceName() const { return impl_->deviceName; }

Status Session::attach(AVCodecContext* codec, const AVCodec* decoder,
                       const std::string& deviceName) {
    if (codec == nullptr || decoder == nullptr) {
        return Status::failure(ErrorCode::InvalidArgument, "No decoder to accelerate");
    }
    if (deviceName.empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "No hardware device was named");
    }

    const AVHWDeviceType type = av_hwdevice_find_type_by_name(deviceName.c_str());
    if (type == AV_HWDEVICE_TYPE_NONE) {
        return Status::failure(ErrorCode::Unsupported,
                               "This build of FFmpeg does not know the accelerator '" +
                                   deviceName + "'");
    }

    const AVPixelFormat hardware = hardwarePixelFormat(decoder, type);
    if (hardware == AV_PIX_FMT_NONE) {
        return Status::failure(
            ErrorCode::Unsupported,
            std::string("This codec cannot be decoded by ") + deviceName + " in this build",
            std::string("codec ") + (decoder->name != nullptr ? decoder->name : "?"));
    }

    AVBufferRef* context = nullptr;
    const int rc = av_hwdevice_ctx_create(&context, type, nullptr, nullptr, 0);
    if (rc < 0 || context == nullptr) {
        return Status::failure(ErrorCode::Unsupported,
                               "The " + deviceName + " device could not be opened",
                               ffmpegErrorString(rc));
    }

    // Nothing about the context is modified until the device is open, so a
    // failure above leaves the caller free to open it for software decoding
    // without rebuilding it.
    impl_->deviceContext = context;
    impl_->deviceName = deviceName;
    impl_->negotiation.hardware = hardware;
    impl_->codec = codec;

    codec->hw_device_ctx = av_buffer_ref(context);
    codec->opaque = &impl_->negotiation;
    codec->get_format = &chooseFormat;
    return Status::success();
}

Status Session::transfer(AVFrame* source, AVFrame* destination, AVFrame** usable) {
    if (source == nullptr || destination == nullptr || usable == nullptr) {
        return Status::failure(ErrorCode::InvalidArgument, "Nothing to transfer");
    }
    // A software frame, either because no device is attached or because
    // negotiation fell back. Nothing to do.
    if (!active() || source->format != impl_->negotiation.hardware) {
        *usable = source;
        return Status::success();
    }

    av_frame_unref(destination);
    const int rc = av_hwframe_transfer_data(destination, source, 0);
    if (rc < 0) {
        // Never fall through to the previous frame: showing one moment of a
        // recording while the position says another is worse than stopping.
        return Status::failure(ErrorCode::MediaError,
                               "A decoded frame could not be read back from the accelerator",
                               ffmpegErrorString(rc));
    }
    // Timestamps live on the hardware frame and do not travel with the pixels.
    const int copied = av_frame_copy_props(destination, source);
    if (copied < 0) {
        return Status::failure(ErrorCode::MediaError,
                               "Frame timing was lost transferring from the accelerator",
                               ffmpegErrorString(copied));
    }
    *usable = destination;
    return Status::success();
}

JsonValue Session::describe() const {
    if (!active()) return JsonValue::null();
    JsonValue value = JsonValue::object().set("device", impl_->deviceName);
    if (const char* format = av_get_pix_fmt_name(impl_->negotiation.hardware); format != nullptr) {
        value.set("hardware_pixel_format", format);
    }
    return value;
}

Result<ComparisonOutcome> verifyMatchesSoftware(const std::filesystem::path& file,
                                                const std::string& deviceName, int frameCount) {
    using ResultType = Result<ComparisonOutcome>;
    if (frameCount <= 0) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "Comparing zero frames proves nothing");
    }

    ComparisonOutcome outcome;
    outcome.deviceName = deviceName;
    outcome.softwareDecoder = "software";

    auto softwareDecoder = VideoDecoder::open(file);
    if (!softwareDecoder) return ResultType(softwareDecoder.error());
    auto acceleratedDecoder = VideoDecoder::openAccelerated(file, deviceName);
    if (!acceleratedDecoder) return ResultType(acceleratedDecoder.error());

    auto software = softwareDecoder.take();
    auto accelerated = acceleratedDecoder.take();
    if (!accelerated->usingHardware()) {
        return ResultType::failure(
            ErrorCode::Unsupported,
            "The accelerator did not take this file, so there is nothing to compare",
            "Decoding fell back to software.");
    }

    std::int64_t totalDifference = 0;
    std::int64_t totalChannels = 0;

    for (int index = 0; index < frameCount; ++index) {
        auto a = software->nextFrame();
        auto b = accelerated->nextFrame();
        if (!a || !b) break;
        const VideoFrameData softwareFrame = a.take();
        const VideoFrameData hardwareFrame = b.take();

        if (softwareFrame.width != hardwareFrame.width ||
            softwareFrame.height != hardwareFrame.height ||
            softwareFrame.rgb.size() != hardwareFrame.rgb.size()) {
            return ResultType::failure(
                ErrorCode::IntegrityFailure,
                "The two decoders produced different frame geometry",
                "This is a larger disagreement than a pixel comparison can describe.");
        }

        ++outcome.framesCompared;
        int worst = 0;
        for (std::size_t byte = 0; byte < softwareFrame.rgb.size(); ++byte) {
            const int difference = std::abs(static_cast<int>(softwareFrame.rgb[byte]) -
                                            static_cast<int>(hardwareFrame.rgb[byte]));
            worst = std::max(worst, difference);
            totalDifference += difference;
        }
        totalChannels += static_cast<std::int64_t>(softwareFrame.rgb.size());
        outcome.maximumChannelDifference = std::max(outcome.maximumChannelDifference, worst);
        if (worst == 0) ++outcome.framesIdentical;
    }

    if (outcome.framesCompared == 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Neither decoder produced a frame to compare");
    }
    outcome.meanChannelDifference =
        totalChannels == 0 ? 0.0
                           : static_cast<double>(totalDifference) /
                                 static_cast<double>(totalChannels);

    logInfo(kComponent, "Hardware decode compared against software",
            JsonValue::object()
                .set("device", deviceName)
                .set("frames", outcome.framesCompared)
                .set("identical", outcome.framesIdentical)
                .set("max_channel_difference", outcome.maximumChannelDifference)
                .set("mean_channel_difference", outcome.meanChannelDifference));
    return ResultType::success(outcome);
}

}  // namespace trace::hwaccel
