#pragma once

#include <QWidget>

#include <memory>
#include <optional>

#include "core/models/evidence.h"
#include "ui/viewer/playback_bridge.h"

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QToolButton;

namespace trace::ui {

class ApplicationContext;
class VideoView;

/// The evidence viewer: transport controls, accurate timecode and frame
/// stepping.
///
/// Positions come from decoded presentation timestamps, so the timecode shown
/// is the timecode of the frame on screen — not an interpolation from a frame
/// counter.
class ViewerPanel : public QWidget {
    Q_OBJECT

public:
    explicit ViewerPanel(ApplicationContext* context, QWidget* parent = nullptr);
    ~ViewerPanel() override;

    PlaybackBridge* playback() const { return playback_.get(); }

    void openEvidence(const Evidence& evidence);
    void closeMedia();

    qint64 position() const;
    qint64 duration() const;
    bool hasMedia() const;

    void togglePlayPause();
    void stepForward();
    void stepBackward();
    void jumpForward();
    void jumpBackward();
    void seek(qint64 positionUs);
    void enterFullScreen();

signals:
    void positionChanged(qint64 positionUs);
    void durationChanged(qint64 durationUs);
    void bookmarkRequested();
    void annotationRequested();
    void frameExportRequested();

private:
    void onFrame(const QImage& image, qint64 positionUs, qint64 frameNumber, bool keyFrame);
    void onStateChanged(int state, const QString& message);
    void updateTimeLabels(qint64 positionUs, qint64 frameNumber);
    void applyControlsEnabled(bool enabled);

    ApplicationContext* context_ = nullptr;
    std::unique_ptr<PlaybackBridge> playback_;
    std::optional<Evidence> evidence_;

    VideoView* view_ = nullptr;
    QWidget* fullScreenWindow_ = nullptr;
    VideoView* fullScreenView_ = nullptr;

    QToolButton* playButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    QToolButton* stepBackButton_ = nullptr;
    QToolButton* stepForwardButton_ = nullptr;
    QToolButton* jumpBackButton_ = nullptr;
    QToolButton* jumpForwardButton_ = nullptr;
    QToolButton* fullScreenButton_ = nullptr;
    QComboBox* speedBox_ = nullptr;
    QLabel* positionLabel_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QToolButton* muteButton_ = nullptr;
    QPushButton* frameExportButton_ = nullptr;
    QPushButton* bookmarkButton_ = nullptr;

    qint64 jumpMicroseconds_ = 5'000'000;
};

}  // namespace trace::ui
