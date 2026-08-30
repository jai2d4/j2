#pragma once

#include <atomic>
#include <optional>

#include "core/common/time_utils.h"

namespace trace {

/// Playback speeds at which the audio track is rendered.
///
/// Only normal speed qualifies. Playing samples faster or slower without
/// resampling shifts their pitch, and a pitch-shifted voice on a recording that
/// may be evidence misrepresents it — an operator would hear a person who does
/// not sound like the person in the room. A video frame shown early is still the
/// frame that was recorded, so video can honestly be sped up; audio cannot, and
/// is silenced instead of being altered.
bool audioPlaysAtSpeed(double speed);

/// Where playback actually is, according to the audio device.
///
/// An audio device consumes samples on its own crystal. That rate is close to
/// the system clock but not equal to it, and over a long recording the
/// difference accumulates into visible lip-sync error. It is also the audio a
/// person notices first: a dropped or repeated video frame passes unremarked
/// where a click or a stutter does not. So when audio is playing, the device is
/// the reference and video follows it.
///
/// The class holds the arithmetic that turns a device's own count — how much it
/// has played since it was told to start — into a position on the media
/// timeline. Getting that offset wrong is what desynchronises a seek, so it
/// lives here where it can be tested without an audio device present.
///
/// Reads and writes come from different threads: the audio worker reports, the
/// playback engine asks. Both are lock-free.
class AudioClock {
public:
    /// Begins at `fromUs` on the media timeline. Until the device reports
    /// something, the clock answers with exactly that: the position it was told
    /// to start from, never an extrapolation.
    void start(Microseconds fromUs);

    /// Stops answering. A stopped clock returns nothing rather than its last
    /// value, so a caller falls back to its own timing instead of pacing against
    /// a position that has quietly stopped advancing.
    void stop();

    /// `playedOutUs` is the device's own count of what it has rendered since it
    /// was started, which is why it is an offset and not a media position.
    ///
    /// Time only moves forward here. A backend that reports a count that jumps
    /// backwards — some do, around an underrun — would otherwise drag video
    /// back with it, so the reported value is clamped to what has already been
    /// seen. A real backwards jump on the media timeline comes from a seek, and
    /// a seek calls start().
    void reportDevicePosition(Microseconds playedOutUs);

    /// The media position the device has reached, or nothing when it is not
    /// running.
    std::optional<Microseconds> positionUs() const;

    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> running_{false};
    std::atomic<Microseconds> originUs_{0};
    std::atomic<Microseconds> playedUs_{0};
};

}  // namespace trace
