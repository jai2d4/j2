#include "media/playback/playback_controller.h"

#include <algorithm>
#include <chrono>

#include "core/common/logging.h"

namespace trace {
namespace {
constexpr const char* kComponent = "playback";
}

const char* toString(PlaybackState state) {
    switch (state) {
        case PlaybackState::Idle:       return "idle";
        case PlaybackState::Opening:    return "opening";
        case PlaybackState::Ready:      return "ready";
        case PlaybackState::Playing:    return "playing";
        case PlaybackState::Paused:     return "paused";
        case PlaybackState::EndOfMedia: return "end_of_media";
        case PlaybackState::Error:      return "error";
    }
    return "idle";
}

const char* toDisplayString(PlaybackState state) {
    switch (state) {
        case PlaybackState::Idle:       return "No media";
        case PlaybackState::Opening:    return "Opening";
        case PlaybackState::Ready:      return "Ready";
        case PlaybackState::Playing:    return "Playing";
        case PlaybackState::Paused:     return "Paused";
        case PlaybackState::EndOfMedia: return "End of media";
        case PlaybackState::Error:      return "Error";
    }
    return "No media";
}

PlaybackController::PlaybackController() {
    running_ = true;
    worker_ = std::thread(&PlaybackController::run, this);
}

PlaybackController::~PlaybackController() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        queue_.push_back(Command{CommandType::Quit, {}, 0, 0, 1.0});
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void PlaybackController::setFrameHandler(FrameHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    frameHandler_ = std::move(handler);
}
void PlaybackController::setStateHandler(StateHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    stateHandler_ = std::move(handler);
}
void PlaybackController::setOpenedHandler(OpenedHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    openedHandler_ = std::move(handler);
}

void PlaybackController::post(Command command) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void PlaybackController::open(const std::filesystem::path& file) {
    setState(PlaybackState::Opening);
    post(Command{CommandType::Open, file, 0, 0, 1.0});
}
void PlaybackController::close() { post(Command{CommandType::Close, {}, 0, 0, 1.0}); }
void PlaybackController::play() { post(Command{CommandType::Play, {}, 0, 0, 1.0}); }
void PlaybackController::pause() { post(Command{CommandType::Pause, {}, 0, 0, 1.0}); }
void PlaybackController::stop() { post(Command{CommandType::Stop, {}, 0, 0, 1.0}); }

void PlaybackController::togglePlayPause() {
    if (state_.load() == PlaybackState::Playing) {
        pause();
    } else {
        play();
    }
}

void PlaybackController::seek(Microseconds positionUs) {
    post(Command{CommandType::Seek, {}, std::max<Microseconds>(0, positionUs), 0, 1.0});
}

void PlaybackController::jump(Microseconds deltaUs) {
    const Microseconds target = std::max<Microseconds>(0, position_.load() + deltaUs);
    seek(target);
}

void PlaybackController::stepForward() { post(Command{CommandType::Step, {}, 0, 1, 1.0}); }
void PlaybackController::stepBackward() { post(Command{CommandType::Step, {}, 0, -1, 1.0}); }

void PlaybackController::setSpeed(double speed) {
    if (speed <= 0.0) return;
    post(Command{CommandType::SetSpeed, {}, 0, 0, speed});
}

std::shared_ptr<const VideoFrameData> PlaybackController::currentFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentFrame_;
}

DecoderStreamInfo PlaybackController::streamInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

std::string PlaybackController::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

bool PlaybackController::waitForState(const std::vector<PlaybackState>& states, int timeoutMs) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return stateChanged_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        const PlaybackState current = state_.load();
        return std::find(states.begin(), states.end(), current) != states.end();
    });
}

bool PlaybackController::waitForIdleQueue(int timeoutMs) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return stateChanged_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  [&] { return queue_.empty() && !processing_; });
}

void PlaybackController::setState(PlaybackState state, std::string message) {
    StateHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!message.empty()) lastError_ = message;
        handler = stateHandler_;
    }
    state_.store(state);
    stateChanged_.notify_all();
    if (handler) handler(state, std::move(message));
}

void PlaybackController::deliverFrame(std::shared_ptr<const VideoFrameData> frame) {
    FrameHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentFrame_ = frame;
        handler = frameHandler_;
    }
    if (frame) position_.store(frame->presentationUs);
    if (handler) handler(std::move(frame));
}

void PlaybackController::presentFrameAt(Microseconds targetUs, bool resetClock) {
    if (decoder_ == nullptr) return;
    auto frame = decoder_->frameAt(targetUs);
    if (!frame) {
        if (frame.error().code() == ErrorCode::NotFound) {
            setState(PlaybackState::EndOfMedia);
            return;
        }
        setState(PlaybackState::Error, frame.error().toString());
        return;
    }
    auto shared = std::make_shared<const VideoFrameData>(frame.take());
    deliverFrame(shared);
    if (resetClock) {
        clockStart_ = std::chrono::steady_clock::now();
        clockStartMediaUs_ = shared->presentationUs;
    }
}

void PlaybackController::handleCommand(const Command& command) {
    switch (command.type) {
        case CommandType::Open: {
            decoder_.reset();
            opened_.store(false);
            position_.store(0);
            duration_.store(0);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                currentFrame_.reset();
                info_ = DecoderStreamInfo{};
                lastError_.clear();
            }
            auto opened = VideoDecoder::open(command.path);
            if (!opened) {
                logError(kComponent, "Unable to open media",
                         JsonValue::object()
                             .set("file", command.path.filename().string())
                             .set("detail", opened.error().toString()));
                setState(PlaybackState::Error, opened.error().toString());
                return;
            }
            decoder_ = opened.take();
            DecoderStreamInfo info = decoder_->info();
            OpenedHandler openedHandler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                info_ = info;
                openedHandler = openedHandler_;
            }
            duration_.store(info.durationUs);
            opened_.store(true);
            presentFrameAt(0, true);
            if (openedHandler) openedHandler(info);
            setState(PlaybackState::Ready);
            return;
        }
        case CommandType::Close:
            decoder_.reset();
            opened_.store(false);
            position_.store(0);
            duration_.store(0);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                currentFrame_.reset();
                info_ = DecoderStreamInfo{};
            }
            setState(PlaybackState::Idle);
            return;

        case CommandType::Play:
            if (decoder_ == nullptr) return;
            if (state_.load() == PlaybackState::EndOfMedia) {
                presentFrameAt(0, true);
            } else {
                clockStart_ = std::chrono::steady_clock::now();
                clockStartMediaUs_ = position_.load();
            }
            setState(PlaybackState::Playing);
            return;

        case CommandType::Pause:
            if (decoder_ == nullptr) return;
            if (state_.load() == PlaybackState::Playing) setState(PlaybackState::Paused);
            return;

        case CommandType::Stop:
            if (decoder_ == nullptr) return;
            presentFrameAt(0, true);
            setState(PlaybackState::Ready);
            return;

        case CommandType::Seek: {
            if (decoder_ == nullptr) return;
            const bool wasPlaying = state_.load() == PlaybackState::Playing;
            presentFrameAt(command.positionUs, true);
            if (!wasPlaying && state_.load() != PlaybackState::Error) {
                setState(PlaybackState::Paused);
            }
            return;
        }

        case CommandType::Step: {
            if (decoder_ == nullptr) return;
            const Microseconds current = position_.load();
            if (command.stepDirection > 0) {
                // Forward stepping decodes the next frame in presentation order
                // rather than adding an assumed frame duration.
                auto frame = decoder_->nextFrame();
                if (!frame) {
                    if (frame.error().code() == ErrorCode::NotFound) {
                        setState(PlaybackState::EndOfMedia);
                    } else {
                        setState(PlaybackState::Error, frame.error().toString());
                    }
                    return;
                }
                deliverFrame(std::make_shared<const VideoFrameData>(frame.take()));
            } else {
                auto frame = decoder_->frameBefore(current);
                if (!frame) {
                    setState(PlaybackState::Error, frame.error().toString());
                    return;
                }
                deliverFrame(std::make_shared<const VideoFrameData>(frame.take()));
            }
            if (state_.load() == PlaybackState::Playing) setState(PlaybackState::Paused);
            else setState(PlaybackState::Paused);
            return;
        }

        case CommandType::SetSpeed:
            speed_.store(command.speed);
            clockStart_ = std::chrono::steady_clock::now();
            clockStartMediaUs_ = position_.load();
            return;

        case CommandType::Quit:
            decoder_.reset();
            return;
    }
}

void PlaybackController::decodeAndPresentNext() {
    if (decoder_ == nullptr) return;
    auto frame = decoder_->nextFrame();
    if (!frame) {
        if (frame.error().code() == ErrorCode::NotFound) {
            setState(PlaybackState::EndOfMedia);
        } else {
            setState(PlaybackState::Error, frame.error().toString());
        }
        return;
    }
    auto shared = std::make_shared<const VideoFrameData>(frame.take());

    // Pace against the frame's own presentation timestamp, scaled by the
    // playback speed. Waiting happens on the condition variable so an incoming
    // command (pause, seek, speed change) interrupts immediately.
    const double speed = std::max(0.01, speed_.load());
    const auto mediaOffset = shared->presentationUs - clockStartMediaUs_;
    const auto targetDelay = std::chrono::microseconds(
        static_cast<std::int64_t>(static_cast<double>(mediaOffset) / speed));
    const auto targetTime = clockStart_ + targetDelay;

    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_until(lock, targetTime, [&] { return !queue_.empty() || !running_; });
    const bool interrupted = !queue_.empty() || !running_;
    lock.unlock();

    if (interrupted && state_.load() != PlaybackState::Playing) return;
    deliverFrame(std::move(shared));
}

void PlaybackController::run() {
    for (;;) {
        Command command;
        bool hasCommand = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (queue_.empty() && state_.load() != PlaybackState::Playing) {
                processing_ = false;
                stateChanged_.notify_all();
                condition_.wait(lock, [&] { return !queue_.empty() || !running_; });
            }
            if (!running_ && queue_.empty()) return;
            if (!queue_.empty()) {
                command = queue_.front();
                queue_.erase(queue_.begin());
                hasCommand = true;
                processing_ = true;
            }
        }

        if (hasCommand) {
            if (command.type == CommandType::Quit) {
                decoder_.reset();
                std::lock_guard<std::mutex> lock(mutex_);
                processing_ = false;
                stateChanged_.notify_all();
                return;
            }
            handleCommand(command);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                processing_ = false;
            }
            stateChanged_.notify_all();
            continue;
        }

        if (state_.load() == PlaybackState::Playing) decodeAndPresentNext();
    }
}

}  // namespace trace
