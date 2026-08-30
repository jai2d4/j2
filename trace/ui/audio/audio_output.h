#pragma once

#include <QObject>
#include <QString>
#include <QThread>

#include <functional>
#include <memory>
#include <optional>

#include "core/common/time_utils.h"
#include "media/audio/audio_clock.h"

namespace trace::ui {

/// Renders the audio track of the item in the viewer, and reports where it has
/// actually got to so video can follow it.
///
/// Decoding and feeding the device happen on a private thread, because an audio
/// device that is not topped up in time produces an audible gap and the GUI
/// thread cannot promise to be free. The GUI-facing methods here queue work onto
/// that thread and return immediately.
///
/// Nothing in this class writes to evidence. It opens the managed original
/// read-only, and volume and muting change what leaves the sound card — never
/// the file, and never anything TRACE stores about it.
///
/// Audio is rendered at normal speed only; see audioPlaysAtSpeed(). At any other
/// speed the track is silenced, the clock stops answering, and video paces itself
/// as it does for an item with no sound.
class AudioOutput : public QObject {
    Q_OBJECT

public:
    explicit AudioOutput(QObject* parent = nullptr);
    ~AudioOutput() override;

    /// True when the machine has an output device TRACE can use. False is a
    /// normal condition — a workstation with no sound card, a locked-down
    /// terminal — and is reported to the operator rather than treated as a
    /// failure of the evidence.
    static bool deviceAvailable();

    /// Prepares the audio track of `absolutePath`. Returns false when the item
    /// has no audio or no device can play it; the reason is available from
    /// unavailableReason().
    bool open(const QString& absolutePath);
    void close();

    /// Begins rendering from `fromUs` on the media timeline. Called on every
    /// seek: the device is restarted rather than kept running, because a device
    /// carrying a second of already-buffered audio would otherwise keep playing
    /// the passage the operator has just left.
    void start(Microseconds fromUs);
    void stop();

    void setVolume(int percent);
    void setMuted(bool muted);
    /// Above or below normal speed the track is silenced rather than altered.
    void setSpeed(double speed);

    bool hasAudio() const { return hasAudio_; }
    QString unavailableReason() const { return unavailableReason_; }

    /// Hand this to PlaybackController::setClockSource(). It answers with the
    /// device's position while audio is playing and with nothing otherwise, and
    /// it is safe to call after this object has been destroyed.
    std::function<std::optional<Microseconds>()> clockSource() const;

private:
    class Engine;

    std::shared_ptr<AudioClock> clock_;
    QThread thread_;
    Engine* engine_ = nullptr;  ///< owned by the thread, deleted when it finishes
    bool hasAudio_ = false;
    QString unavailableReason_;
};

}  // namespace trace::ui
