#include "ui/viewer/playback_bridge.h"

#include <QMetaObject>

namespace trace::ui {

PlaybackBridge::PlaybackBridge(QObject* parent)
    : QObject(parent),
      controller_(std::make_unique<PlaybackController>()),
      audio_(std::make_unique<AudioOutput>(this)) {
    // Video follows the audio device while it is playing. The source answers with
    // nothing when there is no track, no device, or the speed is not normal, and
    // the engine then paces itself exactly as it did before audio existed.
    controller_->setClockSource(audio_->clockSource());

    controller_->setFrameHandler([this](std::shared_ptr<const VideoFrameData> frame) {
        if (!frame || !frame->valid()) return;
        // Converting here keeps the pixel copy off the GUI thread; the QImage
        // owns its buffer so it survives the frame going out of scope.
        QImage image(frame->rgb.data(), frame->width, frame->height, frame->width * 3,
                     QImage::Format_RGB888);
        QImage owned = image.copy();
        const qint64 position = frame->presentationUs;
        const qint64 frameNumber = frame->frameNumber.value_or(-1);
        const bool keyFrame = frame->keyFrame;
        QMetaObject::invokeMethod(
            this,
            [this, owned, position, frameNumber, keyFrame] {
                emit frameReady(owned, position, frameNumber, keyFrame);
            },
            Qt::QueuedConnection);
    });

    controller_->setStateHandler([this](PlaybackState state, std::string message) {
        const QString text = QString::fromStdString(message);
        QMetaObject::invokeMethod(
            this,
            [this, state, text] {
                // Reaching the end or failing stops the device here rather than
                // leaving it to drain: the operator should not still be hearing a
                // recording the viewer has stopped showing.
                if (state == PlaybackState::EndOfMedia || state == PlaybackState::Error) {
                    audio_->stop();
                }
                emit playbackStateChanged(static_cast<int>(state), text);
            },
            Qt::QueuedConnection);
    });

    controller_->setOpenedHandler([this](DecoderStreamInfo info) {
        QMetaObject::invokeMethod(
            this, [this, info] { emit mediaOpened(info.durationUs, info.width, info.height); },
            Qt::QueuedConnection);
    });
}

PlaybackBridge::~PlaybackBridge() {
    // The device stops before the engine does. The other order would leave the
    // playback worker asking a clock whose owner is already being torn down.
    audio_->stop();
}

void PlaybackBridge::restartAudioAt(qint64 positionUs) {
    audio_->stop();
    audio_->start(positionUs);
}

void PlaybackBridge::open(const QString& absolutePath) {
    // Probed before the engine is told to open, so that by the time mediaOpened
    // reaches the viewer, hasAudio() already knows the answer and the transport
    // can be labelled correctly the first time it is drawn.
    audio_->open(absolutePath);
    controller_->open(std::filesystem::path(absolutePath.toStdString()));
}

void PlaybackBridge::close() {
    audio_->close();
    controller_->close();
}

void PlaybackBridge::play() {
    // Playing from the end starts the item again, which the engine handles by
    // returning to zero; audio has to be told the same thing.
    restartAudioAt(state() == PlaybackState::EndOfMedia ? 0 : position());
    controller_->play();
}

void PlaybackBridge::pause() {
    audio_->stop();
    controller_->pause();
}

void PlaybackBridge::togglePlayPause() {
    // Resolved here rather than in the engine so that audio and video make the
    // same decision from the same state.
    if (state() == PlaybackState::Playing) {
        pause();
    } else {
        play();
    }
}

void PlaybackBridge::stop() {
    audio_->stop();
    controller_->stop();
}

void PlaybackBridge::seek(qint64 positionUs) {
    const bool wasPlaying = state() == PlaybackState::Playing;
    controller_->seek(positionUs);
    if (wasPlaying) {
        restartAudioAt(positionUs);
    } else {
        audio_->stop();
    }
}

void PlaybackBridge::jump(qint64 deltaUs) {
    const bool wasPlaying = state() == PlaybackState::Playing;
    const qint64 target = position() + deltaUs;
    controller_->jump(deltaUs);
    if (wasPlaying) {
        restartAudioAt(target < 0 ? 0 : target);
    } else {
        audio_->stop();
    }
}

void PlaybackBridge::stepForward() {
    // Stepping leaves playback paused a frame at a time; there is no passage of
    // audio to render at a standstill.
    audio_->stop();
    controller_->stepForward();
}

void PlaybackBridge::stepBackward() {
    audio_->stop();
    controller_->stepBackward();
}

void PlaybackBridge::setSpeed(double speed) {
    const bool wasPlaying = state() == PlaybackState::Playing;
    audio_->setSpeed(speed);
    controller_->setSpeed(speed);
    // Coming back to normal speed brings the track back with it; leaving normal
    // speed silences it, which setSpeed() has already done.
    if (wasPlaying && audioPlaysAtSpeed(speed)) restartAudioAt(position());
}

bool PlaybackBridge::hasAudio() const { return audio_->hasAudio(); }
QString PlaybackBridge::audioUnavailableReason() const { return audio_->unavailableReason(); }
void PlaybackBridge::setVolume(int percent) { audio_->setVolume(percent); }
void PlaybackBridge::setMuted(bool muted) { audio_->setMuted(muted); }

qint64 PlaybackBridge::position() const { return controller_->position(); }
qint64 PlaybackBridge::duration() const { return controller_->duration(); }
double PlaybackBridge::speed() const { return controller_->speed(); }
bool PlaybackBridge::isOpen() const { return controller_->isOpen(); }
PlaybackState PlaybackBridge::state() const { return controller_->state(); }
DecoderStreamInfo PlaybackBridge::streamInfo() const { return controller_->streamInfo(); }

std::shared_ptr<const VideoFrameData> PlaybackBridge::currentFrame() const {
    return controller_->currentFrame();
}

bool PlaybackBridge::waitForState(const std::vector<PlaybackState>& states, int timeoutMs) const {
    return controller_->waitForState(states, timeoutMs);
}

bool PlaybackBridge::waitForIdle(int timeoutMs) const {
    return controller_->waitForIdleQueue(timeoutMs);
}

}  // namespace trace::ui
