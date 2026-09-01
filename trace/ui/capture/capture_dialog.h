#pragma once

#include <QDialog>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

#include "media/capture/camera.h"
#include "media/capture/capture_service.h"

class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace trace::ui {

class ApplicationContext;
class BackgroundTask;

/// Recording a live camera into the open case.
///
/// ## What the interface has to be honest about
///
/// A capture dialog can mislead in ways an import dialog cannot, and the design
/// is mostly about not doing that:
///
/// - **"Recording" must mean recording.** The state shown comes from packets
///   actually written, not from the fact that a button was pressed. A camera
///   that was reachable a second ago and has now gone quiet shows as
///   disconnected, because an operator standing in front of a scene needs to
///   know that now, not afterwards.
/// - **Gaps are shown while they happen**, not summarised at the end. The gap
///   count is on screen throughout.
/// - **Bluetooth is offered for what it does.** Selecting a Bluetooth camera
///   does not enable Record; it explains that the link carries control, not
///   video, and what to do instead.
/// - **Nothing claims a camera works until it has answered.** Discovery finds
///   candidates; Test is a separate button, and its result is stated rather
///   than assumed.
class CaptureDialog : public QDialog {
    Q_OBJECT

public:
    explicit CaptureDialog(ApplicationContext* context, QWidget* parent = nullptr);
    ~CaptureDialog() override;

    /// Segments that were filed as evidence. Empty when nothing was recorded.
    const std::vector<CapturedEvidence>& captured() const { return captured_; }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void discover();
    void testSelected();
    void startRecording();
    void stopRecording();
    void selectionChanged();
    void updateControls();
    void appendNote(const QString& text);

    /// The camera the operator has chosen, from the list or the address field.
    std::optional<CameraSource> selectedCamera() const;

    ApplicationContext* context_ = nullptr;

    std::vector<CameraSource> found_;
    std::vector<CapturedEvidence> captured_;
    bool recording_ = false;
    bool busy_ = false;

    QListWidget* cameraList_ = nullptr;
    QLineEdit* addressEdit_ = nullptr;
    QLineEdit* descriptionEdit_ = nullptr;
    QSpinBox* durationSpin_ = nullptr;
    QSpinBox* segmentSpin_ = nullptr;
    QCheckBox* localDevicesCheck_ = nullptr;

    QPushButton* discoverButton_ = nullptr;
    QPushButton* testButton_ = nullptr;
    QPushButton* recordButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;

    QLabel* transportLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* elapsedLabel_ = nullptr;
    QLabel* gapsLabel_ = nullptr;
    QProgressBar* activity_ = nullptr;
    QListWidget* notes_ = nullptr;

    std::unique_ptr<BackgroundTask> task_;
};

}  // namespace trace::ui
