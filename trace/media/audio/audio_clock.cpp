#include "media/audio/audio_clock.h"

#include <cmath>

namespace trace {

bool audioPlaysAtSpeed(double speed) {
    // Deliberately an exact-enough comparison rather than a range: 0.99x is not a
    // speed the viewer offers, so anything that is not 1.0 arrived from the speed
    // control and means the operator asked for something other than normal.
    return std::fabs(speed - 1.0) < 1e-6;
}

void AudioClock::start(Microseconds fromUs) {
    // The order matters. Publishing the origin before the running flag means a
    // reader that sees "running" cannot see the previous origin alongside it,
    // which would place playback at the position of the previous seek.
    playedUs_.store(0, std::memory_order_relaxed);
    originUs_.store(fromUs, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
}

void AudioClock::stop() {
    running_.store(false, std::memory_order_release);
}

void AudioClock::reportDevicePosition(Microseconds playedOutUs) {
    if (playedOutUs < 0) return;
    Microseconds seen = playedUs_.load(std::memory_order_relaxed);
    while (playedOutUs > seen) {
        if (playedUs_.compare_exchange_weak(seen, playedOutUs, std::memory_order_relaxed)) {
            return;
        }
    }
}

std::optional<Microseconds> AudioClock::positionUs() const {
    if (!running_.load(std::memory_order_acquire)) return std::nullopt;
    return originUs_.load(std::memory_order_relaxed) + playedUs_.load(std::memory_order_relaxed);
}

}  // namespace trace
