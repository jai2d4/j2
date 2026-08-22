#include "analysis/frame_sampler.h"

#include <algorithm>

namespace trace {

const char* toString(AnalysisQuality quality) {
    switch (quality) {
        case AnalysisQuality::Fast:     return "fast";
        case AnalysisQuality::Standard: return "standard";
        case AnalysisQuality::Detailed: return "detailed";
        case AnalysisQuality::Custom:   return "custom";
    }
    return "standard";
}

const char* toDisplayString(AnalysisQuality quality) {
    switch (quality) {
        case AnalysisQuality::Fast:     return "Fast (~5 frames/s)";
        case AnalysisQuality::Standard: return "Standard (~10 frames/s)";
        case AnalysisQuality::Detailed: return "Detailed (every frame)";
        case AnalysisQuality::Custom:   return "Custom interval";
    }
    return "Standard (~10 frames/s)";
}

AnalysisQuality analysisQualityFromString(const std::string& text, AnalysisQuality fallback) {
    if (text == "fast") return AnalysisQuality::Fast;
    if (text == "standard") return AnalysisQuality::Standard;
    if (text == "detailed") return AnalysisQuality::Detailed;
    if (text == "custom") return AnalysisQuality::Custom;
    return fallback;
}

Microseconds samplingIntervalUs(AnalysisQuality quality, Microseconds customIntervalUs) {
    switch (quality) {
        case AnalysisQuality::Fast:     return 200'000;  // 5 per second
        case AnalysisQuality::Standard: return 100'000;  // 10 per second
        case AnalysisQuality::Detailed: return 0;        // every decoded frame
        case AnalysisQuality::Custom:   return std::max<Microseconds>(0, customIntervalUs);
    }
    return 100'000;
}

FrameSampler::FrameSampler(Microseconds intervalUs)
    : intervalUs_(std::max<Microseconds>(0, intervalUs)) {}

bool FrameSampler::accept(Microseconds presentationUs) {
    if (intervalUs_ <= 0) {
        lastAcceptedUs_ = presentationUs;
        started_ = true;
        return true;
    }
    if (!started_ || presentationUs >= nextDueUs_) {
        started_ = true;
        lastAcceptedUs_ = presentationUs;
        // Spacing is measured from the frame that was actually accepted, so a
        // gap in the recording does not cause a burst of catch-up analysis.
        nextDueUs_ = presentationUs + intervalUs_;
        return true;
    }
    return false;
}

void FrameSampler::reset() {
    nextDueUs_ = 0;
    lastAcceptedUs_ = -1;
    started_ = false;
}

std::optional<std::int64_t> FrameSampler::estimateFrameCount(Microseconds durationUs) const {
    if (durationUs <= 0) return std::nullopt;
    if (intervalUs_ <= 0) return std::nullopt;  // depends on the media's own frame count
    return durationUs / intervalUs_ + 1;
}

}  // namespace trace
