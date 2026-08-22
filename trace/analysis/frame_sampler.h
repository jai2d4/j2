#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/common/time_utils.h"

namespace trace {

/// How densely an analysis samples the media.
///
/// These are target rates, not guarantees: sampling follows the media's own
/// presentation timestamps, so a recording that drops frames is sampled less
/// often in real terms rather than being silently stretched.
enum class AnalysisQuality {
    Fast,      ///< about 5 analysed frames per second of media
    Standard,  ///< about 10 analysed frames per second of media
    Detailed,  ///< every decoded frame
    Custom,    ///< caller-supplied interval
};

const char* toString(AnalysisQuality quality);
const char* toDisplayString(AnalysisQuality quality);
AnalysisQuality analysisQualityFromString(const std::string& text,
                                          AnalysisQuality fallback = AnalysisQuality::Standard);

/// Minimum spacing between analysed frames, in microseconds. Zero means every
/// decoded frame is analysed.
Microseconds samplingIntervalUs(AnalysisQuality quality, Microseconds customIntervalUs = 0);

/// Decides which decoded frames to analyse, from their real timestamps.
///
/// This is the variable-frame-rate rule in practice: a frame is accepted when
/// its presentation timestamp has advanced at least one interval past the last
/// accepted frame. Nothing here divides a frame count by a nominal frame rate,
/// so evidence whose timing is irregular is sampled by time, as recorded.
class FrameSampler {
public:
    explicit FrameSampler(Microseconds intervalUs);

    /// True when this frame should be analysed. Stateful: call once per decoded
    /// frame, in presentation order.
    bool accept(Microseconds presentationUs);
    void reset();

    Microseconds intervalUs() const { return intervalUs_; }
    Microseconds lastAcceptedUs() const { return lastAcceptedUs_; }

    /// How many frames an analysis of `durationUs` is expected to examine, used
    /// only to show a progress estimate.
    std::optional<std::int64_t> estimateFrameCount(Microseconds durationUs) const;

private:
    Microseconds intervalUs_ = 0;
    Microseconds nextDueUs_ = 0;
    Microseconds lastAcceptedUs_ = -1;
    bool started_ = false;
};

}  // namespace trace
