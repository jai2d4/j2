#pragma once

#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

#include "core/models/evidence.h"
#include "ui/common/detection_overlay.h"
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

    // Accessors used by the acceptance test to drive the real UI.
    QToolButton* muteButton() const { return muteButton_; }
    QSlider* volumeSlider() const { return volumeSlider_; }

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

    /// Detections drawn over the frame currently on screen. Display only — the
    /// decoded image and the stored evidence are never touched.
    ///
    /// `analysedFrameUs` is the timestamp of the analysed frame the boxes belong
    /// to, which is rarely exactly the playhead; it is shown to the operator so
    /// nothing pretends the boxes were measured on the frame being displayed.
    void setDetections(std::vector<DetectionOverlayItem> detections,
                       qint64 analysedFrameUs = -1);
    void clearDetections();
    void setSelectedDetection(const QString& detectionId);
    const DetectionOverlayOptions& overlayOptions() const { return overlayOptions_; }
    void setOverlayOptions(const DetectionOverlayOptions& options);
    /// Enables the overlay switches. They stay off while an item has no
    /// analysis results, so nothing offers to show what does not exist.
    void setDetectionOverlayAvailable(bool available);
    bool detectionOverlayAvailable() const { return overlayAvailable_; }

    VideoView* videoView() const { return view_; }

signals:
    void positionChanged(qint64 positionUs);
    void durationChanged(qint64 durationUs);
    void bookmarkRequested();
    void annotationRequested();
    void frameExportRequested();
    /// A box on the frame was clicked; empty means the click missed every box.
    void detectionActivated(const QString& detectionId);
    void overlayOptionsChanged(const DetectionOverlayOptions& options);

private:
    void onFrame(const QImage& image, qint64 positionUs, qint64 frameNumber, bool keyFrame);
    void onStateChanged(int state, const QString& message);
    void updateTimeLabels(qint64 positionUs, qint64 frameNumber);
    void applyControlsEnabled(bool enabled);
    void pushOverlayOptions();
    /// Enables the audio transport for this item, or says why it is off.
    void refreshAudioControls();
    void updateOverlaySummary();

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
    QToolButton* showDetectionsButton_ = nullptr;
    QToolButton* showLabelsButton_ = nullptr;
    QToolButton* showConfidenceButton_ = nullptr;
    QLabel* overlaySummaryLabel_ = nullptr;

    DetectionOverlayOptions overlayOptions_;
    bool overlayAvailable_ = false;
    qint64 overlayFrameUs_ = -1;

    qint64 jumpMicroseconds_ = 5'000'000;
};

}  // namespace trace::ui
