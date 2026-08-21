#include "ui/viewer/playback_bridge.h"

#include <QMetaObject>

namespace trace::ui {

PlaybackBridge::PlaybackBridge(QObject* parent)
    : QObject(parent), controller_(std::make_unique<PlaybackController>()) {
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
            this, [this, state, text] { emit playbackStateChanged(static_cast<int>(state), text); },
            Qt::QueuedConnection);
    });

    controller_->setOpenedHandler([this](DecoderStreamInfo info) {
        QMetaObject::invokeMethod(
            this, [this, info] { emit mediaOpened(info.durationUs, info.width, info.height); },
            Qt::QueuedConnection);
    });
}

PlaybackBridge::~PlaybackBridge() = default;

void PlaybackBridge::open(const QString& absolutePath) {
    controller_->open(std::filesystem::path(absolutePath.toStdString()));
}
void PlaybackBridge::close() { controller_->close(); }
void PlaybackBridge::play() { controller_->play(); }
void PlaybackBridge::pause() { controller_->pause(); }
void PlaybackBridge::togglePlayPause() { controller_->togglePlayPause(); }
void PlaybackBridge::stop() { controller_->stop(); }
void PlaybackBridge::seek(qint64 positionUs) { controller_->seek(positionUs); }
void PlaybackBridge::jump(qint64 deltaUs) { controller_->jump(deltaUs); }
void PlaybackBridge::stepForward() { controller_->stepForward(); }
void PlaybackBridge::stepBackward() { controller_->stepBackward(); }
void PlaybackBridge::setSpeed(double speed) { controller_->setSpeed(speed); }

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
