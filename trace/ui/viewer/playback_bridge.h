#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <memory>

#include "media/playback/playback_controller.h"
#include "ui/audio/audio_output.h"

namespace trace::ui {

/// Qt-facing wrapper around the playback engine.
///
/// The engine is deliberately Qt-free and calls back from its worker thread;
/// this class marshals those callbacks onto the GUI thread as signals, which is
/// the only place widgets are allowed to be touched.
class PlaybackBridge : public QObject {
    Q_OBJECT

public:
    explicit PlaybackBridge(QObject* parent = nullptr);
    ~PlaybackBridge() override;

    void open(const QString& absolutePath);
    void close();

    void play();
    void pause();
    void togglePlayPause();
    void stop();
    void seek(qint64 positionUs);
    void jump(qint64 deltaUs);
    void stepForward();
    void stepBackward();
    void setSpeed(double speed);

    /// True when this item has an audio track and a device that can play it.
    /// When false, audioUnavailableReason() says which, so the viewer can label
    /// its controls with the reason instead of leaving them dead.
    bool hasAudio() const;
    QString audioUnavailableReason() const;
    void setVolume(int percent);
    void setMuted(bool muted);

    qint64 position() const;
    qint64 duration() const;
    double speed() const;
    bool isOpen() const;
    PlaybackState state() const;
    DecoderStreamInfo streamInfo() const;
    std::shared_ptr<const VideoFrameData> currentFrame() const;

    /// Test and shutdown helper — the UI itself never blocks on playback.
    bool waitForState(const std::vector<PlaybackState>& states, int timeoutMs) const;
    bool waitForIdle(int timeoutMs) const;

signals:
    void frameReady(const QImage& image, qint64 positionUs, qint64 frameNumber, bool keyFrame);
    void playbackStateChanged(int state, const QString& message);
    void mediaOpened(qint64 durationUs, int width, int height);

private:
    /// Audio is started fresh at a position rather than left running across a
    /// seek: a device holding buffered audio would otherwise keep playing the
    /// passage the operator has just left.
    void restartAudioAt(qint64 positionUs);

    std::unique_ptr<PlaybackController> controller_;
    std::unique_ptr<AudioOutput> audio_;
};

}  // namespace trace::ui
