#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/common/result.h"
#include "core/common/time_utils.h"
#include "core/security/crypto.h"
#include "media/ffmpeg/video_decoder.h"

namespace trace {

enum class PlaybackState { Idle, Opening, Ready, Playing, Paused, EndOfMedia, Error };

const char* toString(PlaybackState state);
const char* toDisplayString(PlaybackState state);

/// Playback speeds offered by the viewer.
inline constexpr double kPlaybackSpeeds[] = {0.25, 0.5, 1.0, 1.5, 2.0, 4.0};

/// Threaded playback engine.
///
/// All decoding, seeking and pacing happen on a private worker thread; the UI
/// never blocks on a decode. Frames and state changes are delivered through
/// callbacks that fire on that worker thread, so a UI layer must marshal them
/// onto its own thread (the Qt wrapper does exactly that).
///
/// Pacing is driven by real presentation timestamps rather than a frame
/// counter, which keeps variable-frame-rate evidence in sync with its own
/// timeline.
class PlaybackController {
public:
    using FrameHandler = std::function<void(std::shared_ptr<const VideoFrameData>)>;
    using StateHandler = std::function<void(PlaybackState, std::string)>;
    using OpenedHandler = std::function<void(DecoderStreamInfo)>;

    PlaybackController();
    ~PlaybackController();
    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    void setFrameHandler(FrameHandler handler);
    void setStateHandler(StateHandler handler);
    void setOpenedHandler(OpenedHandler handler);

    /// Where playback's idea of "now" comes from while it is playing.
    ///
    /// With no audio there is nothing to follow: frames are paced against their
    /// own presentation timestamps and the steady clock, which is what this
    /// engine has always done. When an audio device is playing, that device
    /// becomes the reference instead and video follows it — see AudioClock for
    /// why the device rather than the system clock gets to be right.
    ///
    /// The callback returns the media position the device has reached, or
    /// nothing when it cannot say: no audio track, not playing, or a speed at
    /// which audio is silenced. Nothing means the steady clock takes over again,
    /// so a file with no sound behaves exactly as it did before.
    ///
    /// Called on the playback worker thread, once per frame. It must not block
    /// and must not call back into this controller.
    using ClockSource = std::function<std::optional<Microseconds>()>;
    void setClockSource(ClockSource source);

    /// Queues an open. Completion arrives as Ready (plus the opened handler) or
    /// Error with a message suitable for display.
    void open(const std::filesystem::path& file);

    /// The key for the evidence being played, when it is stored encrypted.
    ///
    /// Set before open(). Held for the life of the controller rather than
    /// passed with each command, because the worker thread reopens the decoder
    /// on its own — after a seek past the end, or when a command arrives while
    /// the previous file is still closing — and a key that arrived with only
    /// the first command would not be there for the second.
    void setEvidenceKey(std::optional<crypto::SecretKey> key);

    /// Decode through a named accelerator when one is available.
    ///
    /// Empty means software, which is the default. Playback is the one place an
    /// accelerator earns its keep — scrubbing through hours of 4K is decoder
    /// bound and nothing else — and it is also the place where a difference of
    /// one pixel value costs nothing, because what is on screen is a view rather
    /// than an exhibit. Frame export deliberately does not use this: see
    /// docs/HARDWARE_DECODE.md.
    void setHardwareDevice(std::string device);
    void close();

    void play();
    void pause();
    void togglePlayPause();
    /// Stops and returns to the first frame.
    void stop();
    void seek(Microseconds positionUs);
    void jump(Microseconds deltaUs);
    void stepForward();
    void stepBackward();
    void setSpeed(double speed);

    PlaybackState state() const { return state_.load(); }
    Microseconds position() const { return position_.load(); }
    Microseconds duration() const { return duration_.load(); }
    double speed() const { return speed_.load(); }
    bool isOpen() const { return opened_.load(); }

    /// Last frame delivered, for repaints and frame export.
    std::shared_ptr<const VideoFrameData> currentFrame() const;
    DecoderStreamInfo streamInfo() const;
    std::string lastError() const;

    /// Blocks until the controller reaches one of `states` or the timeout
    /// expires. Used by tests and by the shutdown path; the UI never calls it.
    bool waitForState(const std::vector<PlaybackState>& states, int timeoutMs) const;
    /// Blocks until every queued command has been processed.
    bool waitForIdleQueue(int timeoutMs) const;

private:
    enum class CommandType { Open, Close, Play, Pause, Stop, Seek, Step, SetSpeed, Quit };

    /// Guarded by the same mutex as the command queue: set on the GUI thread,
    /// read on the worker.
    std::optional<crypto::SecretKey> evidenceKey_;
    /// Guarded by the same mutex as the command queue.
    std::string hardwareDevice_;

    struct Command {
        CommandType type = CommandType::Play;
        std::filesystem::path path;
        Microseconds positionUs = 0;
        int stepDirection = 0;
        double speed = 1.0;
    };

    void run();
    void post(Command command);
    void handleCommand(const Command& command);
    void deliverFrame(std::shared_ptr<const VideoFrameData> frame);
    void setState(PlaybackState state, std::string message = {});
    void decodeAndPresentNext();
    void presentFrameAt(Microseconds targetUs, bool resetClock);

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::condition_variable stateChanged_;
    std::vector<Command> queue_;
    std::thread worker_;
    bool running_ = false;
    bool processing_ = false;

    std::unique_ptr<VideoDecoder> decoder_;
    DecoderStreamInfo info_;
    std::shared_ptr<const VideoFrameData> currentFrame_;
    std::string lastError_;

    std::atomic<PlaybackState> state_{PlaybackState::Idle};
    std::atomic<Microseconds> position_{0};
    std::atomic<Microseconds> duration_{0};
    std::atomic<double> speed_{1.0};
    std::atomic<bool> opened_{false};

    FrameHandler frameHandler_;
    StateHandler stateHandler_;
    OpenedHandler openedHandler_;
    ClockSource clockSource_;

    std::chrono::steady_clock::time_point clockStart_;
    Microseconds clockStartMediaUs_ = 0;
};

}  // namespace trace
