#pragma once

#include <string>

#include "core/common/json.h"

namespace trace {

/// One-time FFmpeg process setup (log level routing into the TRACE log).
/// Safe to call repeatedly and from any thread.
void initialiseFFmpeg();

/// Human-readable FFmpeg error for a negative return code.
std::string ffmpegErrorString(int errorCode);

/// Library versions in effect, recorded in provenance rows so a derived asset
/// names the exact decoder build that produced it.
JsonValue ffmpegLibraryVersions();

/// Short identifier such as "FFmpeg libavformat 60.16.100".
std::string ffmpegIdentifier();

}  // namespace trace
