#include "ui/capture/capture_dialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "media/capture/bluetooth.h"
#include "media/capture/capture_session.h"
#include "media/capture/discovery.h"
#include "ui/app/application_context.h"
#include "ui/common/background_task.h"
#include "ui/common/display_utils.h"
#include "ui/common/theme.h"

namespace trace::ui {
namespace {

/// Progress arrives from the capture thread far faster than a person can read
/// it. Reported at this interval instead, which is frequent enough that a
/// stalled capture is visible within a second.
constexpr int kProgressIntervalMs = 500;

QString describeCamera(const CameraSource& camera) {
    QString label = QString::fromStdString(camera.name.empty() ? camera.redactedUri() : camera.name);
    const QString transport = QString::fromUtf8(toDisplayString(camera.transport));
    if (!camera.model.empty()) {
        label += QStringLiteral(" (%1)").arg(QString::fromStdString(camera.model));
    }
    return QStringLiteral("%1  ·  %2").arg(label, transport);
}

}  // namespace

CaptureDialog::CaptureDialog(ApplicationContext* context, QWidget* parent)
    : QDialog(parent), context_(context) {
    setWindowTitle(QStringLiteral("Record from a camera"));
    setModal(true);
    setMinimumWidth(680);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("RECORD FROM A CAMERA"), this);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                        "letter-spacing: 1.5px;")
                             .arg(colors::kTextSecondary.name()));
    layout->addWidget(title);

    // The claim this dialog makes about its own output, stated before an
    // operator records anything rather than discovered in a report later.
    auto* preamble = new QLabel(
        QStringLiteral("TRACE is the recording device for a live capture. There is no earlier "
                       "original to check the result against — the recording is filed as "
                       "first-generation evidence, and any interruption is recorded rather than "
                       "closed up."),
        this);
    preamble->setWordWrap(true);
    preamble->setStyleSheet(QStringLiteral("color: %1;").arg(colors::kTextSecondary.name()));
    layout->addWidget(preamble);

    // ------------------------------------------------------------- camera
    auto* sourceBox = new QGroupBox(QStringLiteral("Camera"), this);
    auto* sourceLayout = new QVBoxLayout(sourceBox);

    cameraList_ = new QListWidget(sourceBox);
    cameraList_->setMinimumHeight(110);
    sourceLayout->addWidget(cameraList_);

    auto* discoverRow = new QHBoxLayout();
    discoverButton_ = new QPushButton(QStringLiteral("Search the network"), sourceBox);
    localDevicesCheck_ = new QCheckBox(QStringLiteral("Include attached devices"), sourceBox);
    localDevicesCheck_->setChecked(true);
    testButton_ = new QPushButton(QStringLiteral("Test"), sourceBox);
    discoverRow->addWidget(discoverButton_);
    discoverRow->addWidget(localDevicesCheck_);
    discoverRow->addStretch();
    discoverRow->addWidget(testButton_);
    sourceLayout->addLayout(discoverRow);

    auto* addressForm = new QFormLayout();
    addressEdit_ = new QLineEdit(sourceBox);
    // Entering an address is a first-class path, not a fallback: consumer IP
    // cameras frequently do not answer ONVIF discovery at all.
    addressEdit_->setPlaceholderText(
        QStringLiteral("rtsp://camera.local:554/stream1, http://…, srt://…, or /dev/video0"));
    addressForm->addRow(QStringLiteral("Or enter an address"), addressEdit_);
    sourceLayout->addLayout(addressForm);

    transportLabel_ = new QLabel(sourceBox);
    transportLabel_->setWordWrap(true);
    transportLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(colors::kTextSecondary.name()));
    sourceLayout->addWidget(transportLabel_);

    layout->addWidget(sourceBox);

    // ---------------------------------------------------------- recording
    auto* settingsBox = new QGroupBox(QStringLiteral("Recording"), this);
    auto* settingsForm = new QFormLayout(settingsBox);

    descriptionEdit_ = new QLineEdit(settingsBox);
    descriptionEdit_->setPlaceholderText(QStringLiteral("What is being recorded, and why"));
    settingsForm->addRow(QStringLiteral("Description"), descriptionEdit_);

    durationSpin_ = new QSpinBox(settingsBox);
    durationSpin_->setRange(0, 24 * 60);
    durationSpin_->setValue(0);
    durationSpin_->setSuffix(QStringLiteral(" min"));
    durationSpin_->setSpecialValueText(QStringLiteral("Until stopped"));
    settingsForm->addRow(QStringLiteral("Stop after"), durationSpin_);

    segmentSpin_ = new QSpinBox(settingsBox);
    segmentSpin_->setRange(0, 8192);
    segmentSpin_->setValue(0);
    segmentSpin_->setSuffix(QStringLiteral(" MB"));
    segmentSpin_->setSpecialValueText(QStringLiteral("One file per connection"));
    // Splitting is not a gap and is not recorded as one; it bounds the damage
    // from a truncated write and keeps individual exhibits a workable size.
    settingsForm->addRow(QStringLiteral("Split files at"), segmentSpin_);

    layout->addWidget(settingsBox);

    // ------------------------------------------------------------- status
    auto* statusBox = new QGroupBox(QStringLiteral("Status"), this);
    auto* statusGrid = new QGridLayout(statusBox);

    statusLabel_ = new QLabel(QStringLiteral("Not recording"), statusBox);
    elapsedLabel_ = new QLabel(kNotAvailable, statusBox);
    gapsLabel_ = new QLabel(QStringLiteral("0"), statusBox);

    statusGrid->addWidget(new QLabel(QStringLiteral("Link"), statusBox), 0, 0);
    statusGrid->addWidget(statusLabel_, 0, 1);
    statusGrid->addWidget(new QLabel(QStringLiteral("Recorded"), statusBox), 1, 0);
    statusGrid->addWidget(elapsedLabel_, 1, 1);
    statusGrid->addWidget(new QLabel(QStringLiteral("Interruptions"), statusBox), 2, 0);
    statusGrid->addWidget(gapsLabel_, 2, 1);
    statusGrid->setColumnStretch(1, 1);

    activity_ = new QProgressBar(statusBox);
    activity_->setRange(0, 0);  // indeterminate: a capture has no known length
    activity_->setVisible(false);
    statusGrid->addWidget(activity_, 3, 0, 1, 2);

    notes_ = new QListWidget(statusBox);
    notes_->setMinimumHeight(90);
    statusGrid->addWidget(notes_, 4, 0, 1, 2);

    layout->addWidget(statusBox);

    // ------------------------------------------------------------ buttons
    auto* buttons = new QHBoxLayout();
    recordButton_ = new QPushButton(QStringLiteral("Start recording"), this);
    styleAsPrimaryAction(recordButton_);
    stopButton_ = new QPushButton(QStringLiteral("Stop"), this);
    closeButton_ = new QPushButton(QStringLiteral("Close"), this);
    buttons->addStretch();
    buttons->addWidget(recordButton_);
    buttons->addWidget(stopButton_);
    buttons->addWidget(closeButton_);
    layout->addLayout(buttons);

    connect(discoverButton_, &QPushButton::clicked, this, &CaptureDialog::discover);
    connect(testButton_, &QPushButton::clicked, this, &CaptureDialog::testSelected);
    connect(recordButton_, &QPushButton::clicked, this, &CaptureDialog::startRecording);
    connect(stopButton_, &QPushButton::clicked, this, &CaptureDialog::stopRecording);
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::accept);
    connect(cameraList_, &QListWidget::currentRowChanged, this,
            [this](int) { selectionChanged(); });
    connect(addressEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        // Typing an address means the operator is not using the list.
        if (!addressEdit_->text().trimmed().isEmpty()) cameraList_->clearSelection();
        selectionChanged();
    });

    selectionChanged();
}

CaptureDialog::~CaptureDialog() {
    if (task_ && task_->running()) {
        context_->capture().stop();
        task_->wait();
    }
}

void CaptureDialog::closeEvent(QCloseEvent* event) {
    if (recording_) {
        // Closing while recording would leave a capture running with nothing
        // watching it. The operator is asked, and the capture is stopped
        // cleanly — everything recorded so far is still filed.
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Stop the recording?"),
            QStringLiteral("A capture is still running. Closing this window stops it. "
                           "Everything recorded so far is kept and filed."),
            QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        stopRecording();
        if (task_) task_->wait();
    }
    QDialog::closeEvent(event);
}

std::optional<CameraSource> CaptureDialog::selectedCamera() const {
    const QString typed = addressEdit_->text().trimmed();
    if (!typed.isEmpty()) {
        auto parsed = cameraSourceFromUri(typed.toStdString());
        if (!parsed) return std::nullopt;
        return parsed.take();
    }
    const int row = cameraList_->currentRow();
    if (row < 0 || row >= static_cast<int>(found_.size())) return std::nullopt;
    return found_[static_cast<std::size_t>(row)];
}

void CaptureDialog::selectionChanged() {
    const QString typed = addressEdit_->text().trimmed();
    const auto camera = selectedCamera();

    if (!camera) {
        if (typed.isEmpty()) {
            transportLabel_->setText(QStringLiteral("Choose a camera or enter an address."));
        } else {
            // The refusal names what would work, rather than leaving avformat to
            // fail later with something an operator cannot act on.
            auto parsed = cameraSourceFromUri(typed.toStdString());
            transportLabel_->setText(
                QStringLiteral("%1 %2")
                    .arg(QString::fromStdString(parsed.error().message()),
                         QString::fromStdString(parsed.error().detail())));
        }
        updateControls();
        return;
    }

    if (!carriesVideo(camera->transport)) {
        // Not a dead end: the useful thing Bluetooth does is named here.
        transportLabel_->setText(
            QStringLiteral("%1\n\nUse the Bluetooth link to have the camera start streaming, then "
                           "record from the address it provides.")
                .arg(QString::fromUtf8(bluetooth::videoOverBluetoothExplanation())));
    } else {
        transportLabel_->setText(
            QStringLiteral("%1 over %2. Nothing has been checked yet — press Test to find out "
                           "whether it answers.")
                .arg(QString::fromUtf8(toDisplayString(camera->transport)),
                     QString::fromUtf8(toDisplayString(camera->link))));
    }
    updateControls();
}

void CaptureDialog::updateControls() {
    const auto camera = selectedCamera();
    const bool usable = camera.has_value() && carriesVideo(camera->transport);
    const bool idle = !recording_ && !busy_;

    discoverButton_->setEnabled(idle);
    localDevicesCheck_->setEnabled(idle);
    addressEdit_->setEnabled(idle);
    cameraList_->setEnabled(idle);
    descriptionEdit_->setEnabled(idle);
    durationSpin_->setEnabled(idle);
    segmentSpin_->setEnabled(idle);
    testButton_->setEnabled(idle && usable);
    recordButton_->setEnabled(idle && usable && context_->currentCase().has_value());
    stopButton_->setEnabled(recording_);
    closeButton_->setEnabled(!busy_ || recording_);
    activity_->setVisible(recording_);
}

void CaptureDialog::appendNote(const QString& text) {
    notes_->addItem(text);
    notes_->scrollToBottom();
}

void CaptureDialog::discover() {
    busy_ = true;
    updateControls();
    appendNote(QStringLiteral("Searching for cameras…"));

    discovery::Options options;
    options.includeLocalDevices = localDevicesCheck_->isChecked();

    task_ = std::make_unique<BackgroundTask>();
    // Shared rather than owned by either lambda: the worker fills them and the
    // GUI thread reads them, and whichever outlives the other frees them. A raw
    // pointer deleted in the finished slot would leak if the task were destroyed
    // with that signal still queued.
    auto found = std::make_shared<std::vector<CameraSource>>();
    auto unavailable = std::make_shared<std::vector<std::pair<std::string, std::string>>>();

    connect(task_.get(), &BackgroundTask::finished, this,
            [this, found, unavailable](bool, const QString&) {
                found_ = *found;
                cameraList_->clear();
                for (const auto& camera : found_) {
                    cameraList_->addItem(describeCamera(camera));
                }
                // The half that a plain list would hide: which transports could
                // not be searched, and why.
                for (const auto& [transport, reason] : *unavailable) {
                    appendNote(QStringLiteral("Could not search %1: %2")
                                   .arg(QString::fromStdString(transport),
                                        QString::fromStdString(reason)));
                }
                appendNote(found_.empty()
                               ? QStringLiteral("No cameras answered. Enter an address if you "
                                                "know one — many cameras do not announce "
                                                "themselves.")
                               : QStringLiteral("Found %1 camera(s). None has been contacted yet.")
                                     .arg(found_.size()));
                busy_ = false;
                updateControls();
            });

    task_->start([options, found, unavailable](BackgroundTask& task) {
        auto outcome = discovery::findCameras(options);
        if (outcome) {
            *found = outcome.value().cameras;
            *unavailable = outcome.value().unavailable;
            task.reportFinished(true, {});
        } else {
            unavailable->emplace_back("cameras", outcome.error().message());
            task.reportFinished(false, {});
        }
    });
}

void CaptureDialog::testSelected() {
    const auto camera = selectedCamera();
    if (!camera) return;

    busy_ = true;
    updateControls();
    appendNote(QStringLiteral("Testing %1…").arg(QString::fromStdString(camera->redactedUri())));

    task_ = std::make_unique<BackgroundTask>();
    connect(task_.get(), &BackgroundTask::finished, this,
            [this](bool succeeded, const QString& message) {
                appendNote(succeeded ? QStringLiteral("The camera answered and has video.")
                                     : QStringLiteral("No video from that camera: %1").arg(message));
                busy_ = false;
                updateControls();
            });

    const CameraSource target = *camera;
    task_->start([target](BackgroundTask& task) {
        const Status status = probeCamera(target, 8);
        task.reportFinished(status.ok(),
                            status.ok() ? QString()
                                        : QString::fromStdString(status.error().toString()));
    });
}

void CaptureDialog::startRecording() {
    const auto camera = selectedCamera();
    if (!camera || !context_->currentCase()) return;

    CaptureRegistration request;
    request.caseId = context_->currentCase()->id;
    request.caseNumber = context_->currentCase()->caseNumber;
    request.camera = *camera;
    request.description = descriptionEdit_->text().trimmed().toStdString();
    request.settings.maximumDurationMs =
        static_cast<std::int64_t>(durationSpin_->value()) * 60 * 1000;
    request.settings.segmentBytes =
        static_cast<std::int64_t>(segmentSpin_->value()) * 1024 * 1024;

    recording_ = true;
    updateControls();
    statusLabel_->setText(QStringLiteral("Connecting…"));
    appendNote(QStringLiteral("Recording started from %1")
                   .arg(QString::fromStdString(camera->redactedUri())));

    task_ = std::make_unique<BackgroundTask>();

    connect(task_.get(), &BackgroundTask::progressChanged, this,
            [this](int gaps, const QString& connected, const QString& message) {
                // "Recording" here means packets have been written, not that a
                // button was pressed. A camera that has gone quiet says so.
                const bool live = connected == QStringLiteral("1");
                statusLabel_->setText(live ? QStringLiteral("Recording")
                                           : QStringLiteral("Not receiving video"));
                statusLabel_->setStyleSheet(
                    QStringLiteral("color: %1;")
                        .arg((live ? colors::kVerified : colors::kFailure).name()));
                elapsedLabel_->setText(message);
                gapsLabel_->setText(QString::number(gaps));
                if (gaps > 0) {
                    gapsLabel_->setStyleSheet(
                        QStringLiteral("color: %1;").arg(colors::kFailure.name()));
                }
            });

    connect(task_.get(), &BackgroundTask::finished, this,
            [this](bool succeeded, const QString& message) {
                recording_ = false;
                statusLabel_->setText(succeeded ? QStringLiteral("Finished")
                                                : QStringLiteral("Stopped without recording"));
                statusLabel_->setStyleSheet(QString());
                appendNote(message);
                context_->notifyEvidenceChanged();
                context_->notifyAuditChanged();
                updateControls();
            });

    auto* context = context_;
    auto* captured = &captured_;
    task_->start([context, request, captured](BackgroundTask& task) {
        std::int64_t lastReportMs = 0;
        auto outcome = context->capture().capture(
            request, [&task, &lastReportMs](const CaptureProgress& progress) {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
                if (now - lastReportMs >= kProgressIntervalMs) {
                    lastReportMs = now;
                    task.reportProgress(
                        progress.gapsSoFar, progress.connected ? QStringLiteral("1")
                                                               : QStringLiteral("0"),
                        QStringLiteral("%1  ·  %2  ·  %3 frames")
                            .arg(timecode(progress.segmentDurationUs),
                                 fileSize(progress.bytesWritten),
                                 QString::number(progress.framesWritten)));
                }
                return !task.cancellationRequested();
            });

        if (!outcome) {
            task.reportFinished(false, QString::fromStdString(outcome.error().toString()));
            return;
        }
        const CaptureRegistrationOutcome result = outcome.take();
        *captured = result.items;

        QString message = QStringLiteral("Filed %1 recording(s).").arg(result.items.size());
        for (const auto& item : result.items) {
            message += QStringLiteral("\n%1 — %2, SHA-256 %3…")
                           .arg(QString::fromStdString(item.evidence.evidenceNumber),
                                timecode(item.segment.durationUs),
                                QString::fromStdString(item.segment.sha256.substr(0, 16)));
        }
        // Stated at the end as well as during, because this is the sentence
        // that goes into the operator's memory of what they recorded.
        if (!result.capture.continuous()) {
            message += QStringLiteral("\n\nThe recording was interrupted %1 time(s). The missing "
                                      "time is recorded in each exhibit's provenance and has not "
                                      "been closed up.")
                           .arg(result.capture.gaps.size());
        }
        for (const auto& [path, reason] : result.unregistered) {
            message += QStringLiteral("\n\nA recorded file could not be filed and has been kept "
                                      "at %1: %2")
                           .arg(QString::fromStdString(path.string()),
                                QString::fromStdString(reason));
        }
        task.reportFinished(!result.items.empty(), message);
    });
}

void CaptureDialog::stopRecording() {
    if (!recording_) return;
    appendNote(QStringLiteral("Stopping — the current file is closed properly and hashed."));
    context_->capture().stop();
    if (task_) task_->requestCancellation();
    stopButton_->setEnabled(false);
}

}  // namespace trace::ui
