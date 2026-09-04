#include "media/ffmpeg/ffmpeg_support.h"

#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "core/common/logging.h"

namespace trace {
namespace {

constexpr const char* kComponent = "ffmpeg";

std::string versionString(unsigned int version) {
    return std::to_string(AV_VERSION_MAJOR(version)) + "." +
           std::to_string(AV_VERSION_MINOR(version)) + "." +
           std::to_string(AV_VERSION_MICRO(version));
}

/// FFmpeg writes decoder diagnostics through this callback; routing it into the
/// TRACE log means a codec problem shows up in the same place as everything
/// else instead of on stderr.
void logCallback(void* /*avcl*/, int level, const char* format, va_list args) {
    if (level > AV_LOG_WARNING) return;
    char buffer[1024];
    int printPrefix = 1;
    const int written = av_log_format_line2(nullptr, level, format, args, buffer,
                                            static_cast<int>(sizeof(buffer)), &printPrefix);
    if (written <= 0) return;
    std::string message(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    if (message.empty()) return;
    if (level <= AV_LOG_ERROR) {
        logError(kComponent, message);
    } else {
        logWarn(kComponent, message);
    }
}

}  // namespace

void initialiseFFmpeg() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(logCallback);
        logInfo(kComponent, "FFmpeg initialised", ffmpegLibraryVersions());
    });
}

std::string ffmpegErrorString(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(errorCode, buffer, sizeof(buffer)) < 0) {
        return "FFmpeg error " + std::to_string(errorCode);
    }
    return std::string(buffer);
}

JsonValue ffmpegLibraryVersions() {
    return JsonValue::object()
        .set("libavformat", versionString(avformat_version()))
        .set("libavcodec", versionString(avcodec_version()))
        .set("libavutil", versionString(avutil_version()))
        .set("libswscale", versionString(swscale_version()))
        .set("libswresample", versionString(swresample_version()));
}

std::string ffmpegIdentifier() {
    return "FFmpeg libavformat " + versionString(avformat_version());
}

}  // namespace trace
