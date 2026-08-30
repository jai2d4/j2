#include "ui/viewer/viewer_panel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/settings/settings_service.h"
#include "ui/app/application_context.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"
#include "ui/viewer/video_view.h"

namespace trace::ui {
namespace {

QToolButton* makeTransportButton(QWidget* parent, const QString& text, const QString& tooltip) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tooltip);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setAutoRaise(true);
    button->setMinimumWidth(46);
    button->setMinimumHeight(28);
    button->setEnabled(false);
    return button;
}

QToolButton* makeOverlayToggle(QWidget* parent, const QString& text, const QString& tooltip) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tooltip);
    button->setCheckable(true);
    button->setChecked(true);
    button->setAutoRaise(true);
    button->setMinimumHeight(24);
    button->setEnabled(false);
    return button;
}

/// Full-screen viewer window: shows the same frames and nothing else, so the
/// analyst sees the image without any chrome over it.
class FullScreenWindow : public QWidget {
public:
    explicit FullScreenWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
        setWindowTitle(QStringLiteral("TRACE — full screen viewer"));
        setStyleSheet(QStringLiteral("background-color: #05070a;"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        view = new VideoView(this);
        view->setPlaceholder(QStringLiteral("Press Esc to leave full screen"));
        layout->addWidget(view);
    }

    VideoView* view = nullptr;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_F11) {
            close();
            return;
        }
        QWidget::keyPressEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { close(); }
};

}  // namespace

ViewerPanel::ViewerPanel(ApplicationContext* context, QWidget* parent)
    : QWidget(parent), context_(context), playback_(std::make_unique<PlaybackBridge>()) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    view_ = new VideoView(this);
    layout->addWidget(view_, 1);

    auto* transport = new QWidget(this);
    transport->setStyleSheet(QStringLiteral("background-color: %1; border-top: 1px solid %2;")
                                 .arg(colors::kSurface.name(), colors::kBorder.name()));
    auto* transportLayout = new QHBoxLayout(transport);
    transportLayout->setContentsMargins(10, 6, 10, 6);
    transportLayout->setSpacing(6);

    jumpBackButton_ = makeTransportButton(transport, QStringLiteral("« 5s"),
                                          QStringLiteral("Jump backward (Shift+Left)"));
    stepBackButton_ = makeTransportButton(transport, QStringLiteral("◀|"),
                                          QStringLiteral("Previous frame (Left)"));
    playButton_ = makeTransportButton(transport, QStringLiteral("▶"),
                                      QStringLiteral("Play / pause (Space)"));
    stopButton_ = makeTransportButton(transport, QStringLiteral("■"),
                                      QStringLiteral("Stop and return to the first frame"));
    stepForwardButton_ = makeTransportButton(transport, QStringLiteral("|▶"),
                                             QStringLiteral("Next frame (Right)"));
    jumpForwardButton_ = makeTransportButton(transport, QStringLiteral("5s »"),
                                             QStringLiteral("Jump forward (Shift+Right)"));

    for (auto* button : {jumpBackButton_, stepBackButton_, playButton_, stopButton_,
                         stepForwardButton_, jumpForwardButton_}) {
        transportLayout->addWidget(button);
    }

    transportLayout->addSpacing(12);
    auto* speedLabel = new QLabel(QStringLiteral("Speed"), transport);
    speedLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    transportLayout->addWidget(speedLabel);
    speedBox_ = new QComboBox(transport);
    for (const double speed : kPlaybackSpeeds) {
        speedBox_->addItem(QStringLiteral("%1x").arg(speed, 0, 'g', 3), speed);
    }
    speedBox_->setCurrentIndex(2);  // 1.0x
    speedBox_->setEnabled(false);
    speedBox_->setFixedWidth(76);
    transportLayout->addWidget(speedBox_);

    transportLayout->addSpacing(16);
    positionLabel_ = new QLabel(QStringLiteral("--:--:--.---"), transport);
    positionLabel_->setFont(monospaceFont());
    positionLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 600;").arg(colors::kTextPrimary.name()));
    transportLayout->addWidget(positionLabel_);

    auto* separator = new QLabel(QStringLiteral("/"), transport);
    separator->setStyleSheet(QStringLiteral("color: %1;").arg(colors::kTextDisabled.name()));
    transportLayout->addWidget(separator);

    durationLabel_ = new QLabel(QStringLiteral("--:--:--.---"), transport);
    durationLabel_->setFont(monospaceFont());
    durationLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(colors::kTextSecondary.name()));
    transportLayout->addWidget(durationLabel_);

    transportLayout->addSpacing(14);
    frameLabel_ = new QLabel(QStringLiteral("Frame —"), transport);
    frameLabel_->setFont(monospaceFont());
    frameLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(colors::kTextSecondary.name()));
    transportLayout->addWidget(frameLabel_);

    transportLayout->addStretch();

    // Audio transport. Enabled per item by refreshAudioControls(): an item with
    // no track, or a machine with no output device, leaves these off and says
    // which in the tooltip rather than looking broken.
    muteButton_ = makeTransportButton(transport, QStringLiteral("🔇"), QString());
    muteButton_->setCheckable(true);
    muteButton_->setEnabled(false);
    volumeSlider_ = new QSlider(Qt::Horizontal, transport);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(
        static_cast<int>(context_->settings().getInt(settings_keys::kPlaybackVolume, 80)));
    volumeSlider_->setFixedWidth(90);
    volumeSlider_->setEnabled(false);
    transportLayout->addWidget(muteButton_);
    transportLayout->addWidget(volumeSlider_);

    transportLayout->addSpacing(10);
    fullScreenButton_ = makeTransportButton(transport, QStringLiteral("⛶"),
                                            QStringLiteral("Full screen viewer (F11)"));
    transportLayout->addWidget(fullScreenButton_);

    layout->addWidget(transport);

    auto* actions = new QWidget(this);
    actions->setStyleSheet(QStringLiteral("background-color: %1; border-top: 1px solid %2;")
                               .arg(colors::kSurface.name(), colors::kBorder.name()));
    auto* actionLayout = new QHBoxLayout(actions);
    actionLayout->setContentsMargins(10, 6, 10, 6);
    actionLayout->setSpacing(8);

    stateLabel_ = new QLabel(QStringLiteral("No media"), actions);
    stateLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px; letter-spacing: 1px;")
            .arg(colors::kTextSecondary.name()));
    actionLayout->addWidget(stateLabel_);

    actionLayout->addSpacing(16);
    showDetectionsButton_ = makeOverlayToggle(
        actions, QStringLiteral("Show AI detections"),
        QStringLiteral("Draw the stored detection boxes over the frame. The overlay is drawn on "
                       "screen only — the evidence file is never altered."));
    showLabelsButton_ = makeOverlayToggle(actions, QStringLiteral("Labels"),
                                          QStringLiteral("Show the detected class name"));
    showConfidenceButton_ =
        makeOverlayToggle(actions, QStringLiteral("Confidence"),
                          QStringLiteral("Show the model's confidence score for each box"));
    actionLayout->addWidget(showDetectionsButton_);
    actionLayout->addWidget(showLabelsButton_);
    actionLayout->addWidget(showConfidenceButton_);

    overlaySummaryLabel_ = new QLabel(QString(), actions);
    overlaySummaryLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    actionLayout->addSpacing(10);
    actionLayout->addWidget(overlaySummaryLabel_);

    actionLayout->addStretch();

    frameExportButton_ = new QPushButton(QStringLiteral("Save current frame"), actions);
    frameExportButton_->setToolTip(
        QStringLiteral("Write the frame on screen to the case exports folder as a derived asset "
                       "with full provenance"));
    frameExportButton_->setEnabled(false);
    bookmarkButton_ = new QPushButton(QStringLiteral("Add bookmark"), actions);
    styleAsPrimaryAction(bookmarkButton_);
    bookmarkButton_->setEnabled(false);
    actionLayout->addWidget(frameExportButton_);
    actionLayout->addWidget(bookmarkButton_);
    layout->addWidget(actions);

    connect(playback_.get(), &PlaybackBridge::frameReady, this, &ViewerPanel::onFrame);
    connect(playback_.get(), &PlaybackBridge::playbackStateChanged, this,
            &ViewerPanel::onStateChanged);
    connect(playback_.get(), &PlaybackBridge::mediaOpened, this,
            [this](qint64 durationUs, int, int) {
                durationLabel_->setText(timecode(durationUs));
                emit durationChanged(durationUs);
            });

    connect(playButton_, &QToolButton::clicked, this, &ViewerPanel::togglePlayPause);
    connect(stopButton_, &QToolButton::clicked, this, [this] { playback_->stop(); });
    connect(stepBackButton_, &QToolButton::clicked, this, &ViewerPanel::stepBackward);
    connect(stepForwardButton_, &QToolButton::clicked, this, &ViewerPanel::stepForward);
    connect(jumpBackButton_, &QToolButton::clicked, this, &ViewerPanel::jumpBackward);
    connect(jumpForwardButton_, &QToolButton::clicked, this, &ViewerPanel::jumpForward);
    connect(fullScreenButton_, &QToolButton::clicked, this, &ViewerPanel::enterFullScreen);
    connect(speedBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
        playback_->setSpeed(speedBox_->itemData(index).toDouble());
    });
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        // Changes what leaves the sound card and what the next item opens at.
        // Neither touches the recording.
        playback_->setVolume(value);
        context_->settings().setInt(settings_keys::kPlaybackVolume, value);
    });
    connect(muteButton_, &QToolButton::toggled, this, [this](bool muted) {
        playback_->setMuted(muted);
        context_->settings().setBool(settings_keys::kPlaybackMuted, muted);
        muteButton_->setText(muted ? QStringLiteral("🔇") : QStringLiteral("🔊"));
    });
    connect(frameExportButton_, &QPushButton::clicked, this,
            [this] { emit frameExportRequested(); });
    connect(bookmarkButton_, &QPushButton::clicked, this, [this] { emit bookmarkRequested(); });

    for (auto* toggle : {showDetectionsButton_, showLabelsButton_, showConfidenceButton_}) {
        connect(toggle, &QToolButton::toggled, this, [this] { pushOverlayOptions(); });
    }
    connect(view_, &VideoView::detectionClicked, this,
            [this](const QString& id) { emit detectionActivated(id); });
}

ViewerPanel::~ViewerPanel() {
    if (fullScreenWindow_ != nullptr) fullScreenWindow_->close();
    playback_->close();
}

void ViewerPanel::applyControlsEnabled(bool enabled) {
    for (auto* button : {jumpBackButton_, stepBackButton_, playButton_, stopButton_,
                         stepForwardButton_, jumpForwardButton_, fullScreenButton_}) {
        button->setEnabled(enabled);
    }
    speedBox_->setEnabled(enabled);
    frameExportButton_->setEnabled(enabled);
    bookmarkButton_->setEnabled(enabled);
    if (!enabled) {
        setDetectionOverlayAvailable(false);
        refreshAudioControls();
    }
}

void ViewerPanel::openEvidence(const Evidence& evidence) {
    // Re-opening what is already on screen would discard the analyst's
    // position for no reason.
    if (evidence_ && evidence_->id == evidence.id && playback_->isOpen()) return;

    evidence_ = evidence;

    jumpMicroseconds_ =
        context_->settings().getInt(settings_keys::kPlaybackJumpSeconds, 5) * 1'000'000;
    const QString jumpText = QStringLiteral("%1s").arg(jumpMicroseconds_ / 1'000'000);
    jumpBackButton_->setText(QStringLiteral("« ") + jumpText);
    jumpForwardButton_->setText(jumpText + QStringLiteral(" »"));

    if (evidence.mediaType != MediaType::Video && evidence.mediaType != MediaType::Image) {
        // Audio-only items are decoded and their waveform is built, but the viewer
        // itself shows a picture; there is no picture here to show.
        view_->clear(QStringLiteral("%1 is %2 — the viewer plays video.")
                         .arg(QString::fromStdString(evidence.evidenceNumber),
                              QString::fromUtf8(toDisplayString(evidence.mediaType)).toLower()));
        applyControlsEnabled(false);
        stateLabel_->setText(QStringLiteral("Not a video item"));
        playback_->close();
        return;
    }

    view_->clear(QStringLiteral("Opening %1…").arg(QString::fromStdString(evidence.originalFilename)));
    positionLabel_->setText(QStringLiteral("--:--:--.---"));
    durationLabel_->setText(QStringLiteral("--:--:--.---"));
    frameLabel_->setText(QStringLiteral("Frame —"));
    stateLabel_->setText(QStringLiteral("Opening"));

    const auto path = context_->evidence().absolutePath(evidence);
    playback_->open(QString::fromStdString(path.string()));

    // open() probes the track, so by here the answer is known and the transport
    // can be labelled correctly the first time it is drawn.
    const bool startMuted = context_->settings().getBool(settings_keys::kPlaybackMuted, false);
    muteButton_->setChecked(startMuted);
    muteButton_->setText(startMuted ? QStringLiteral("🔇") : QStringLiteral("🔊"));
    playback_->setMuted(startMuted);
    playback_->setVolume(volumeSlider_->value());
    refreshAudioControls();

    const double defaultSpeed =
        context_->settings().getDouble(settings_keys::kPlaybackDefaultSpeed, 1.0);
    for (int i = 0; i < speedBox_->count(); ++i) {
        if (qFuzzyCompare(speedBox_->itemData(i).toDouble(), defaultSpeed)) {
            speedBox_->setCurrentIndex(i);
            break;
        }
    }
}

void ViewerPanel::refreshAudioControls() {
    const bool available = playback_->isOpen() || playback_->hasAudio();
    const bool usable = playback_->hasAudio();
    muteButton_->setEnabled(usable);
    volumeSlider_->setEnabled(usable);

    if (usable) {
        const QString tip = QStringLiteral(
            "Audio plays at normal speed only. At any other speed the track is silenced "
            "rather than pitch-shifted, because a pitch-shifted voice misrepresents the "
            "recording.");
        muteButton_->setToolTip(tip);
        volumeSlider_->setToolTip(tip);
        return;
    }

    // Say which of the two reasons it is. "No audio" is a fact about the evidence;
    // "no device" is a fact about this machine, and an analyst needs to be able to
    // tell them apart before concluding a recording is silent.
    QString reason = playback_->audioUnavailableReason();
    if (reason.isEmpty()) {
        reason = available ? QStringLiteral("This item has no audio track.")
                           : QStringLiteral("No evidence is open.");
    }
    reason += QStringLiteral(" Audio stream details, when there are any, are shown in the "
                             "inspector.");
    muteButton_->setToolTip(reason);
    volumeSlider_->setToolTip(reason);
}

void ViewerPanel::setDetections(std::vector<DetectionOverlayItem> detections,
                                qint64 analysedFrameUs) {
    overlayFrameUs_ = analysedFrameUs;
    view_->setDetections(detections);
    if (fullScreenView_ != nullptr) fullScreenView_->setDetections(detections);
    updateOverlaySummary();
}

void ViewerPanel::clearDetections() {
    overlayFrameUs_ = -1;
    view_->clearDetections();
    if (fullScreenView_ != nullptr) fullScreenView_->clearDetections();
    updateOverlaySummary();
}

void ViewerPanel::setSelectedDetection(const QString& detectionId) {
    view_->setSelectedDetection(detectionId);
    if (fullScreenView_ != nullptr) fullScreenView_->setSelectedDetection(detectionId);
}

void ViewerPanel::setOverlayOptions(const DetectionOverlayOptions& options) {
    overlayOptions_ = options;
    const QSignalBlocker blockBoxes(showDetectionsButton_);
    const QSignalBlocker blockLabels(showLabelsButton_);
    const QSignalBlocker blockConfidence(showConfidenceButton_);
    showDetectionsButton_->setChecked(options.showBoxes);
    showLabelsButton_->setChecked(options.showLabels);
    showConfidenceButton_->setChecked(options.showConfidence);
    showLabelsButton_->setEnabled(overlayAvailable_ && options.showBoxes);
    showConfidenceButton_->setEnabled(overlayAvailable_ && options.showBoxes);

    view_->setOverlayOptions(options);
    if (fullScreenView_ != nullptr) fullScreenView_->setOverlayOptions(options);
    updateOverlaySummary();
}

void ViewerPanel::setDetectionOverlayAvailable(bool available) {
    overlayAvailable_ = available;
    showDetectionsButton_->setEnabled(available);
    showLabelsButton_->setEnabled(available && overlayOptions_.showBoxes);
    showConfidenceButton_->setEnabled(available && overlayOptions_.showBoxes);
    if (!available) clearDetections();
    updateOverlaySummary();
}

void ViewerPanel::pushOverlayOptions() {
    DetectionOverlayOptions options;
    options.showBoxes = showDetectionsButton_->isChecked();
    options.showLabels = showLabelsButton_->isChecked();
    options.showConfidence = showConfidenceButton_->isChecked();
    setOverlayOptions(options);
    emit overlayOptionsChanged(options);
}

void ViewerPanel::updateOverlaySummary() {
    if (!overlayAvailable_) {
        overlaySummaryLabel_->setText(QString());
        return;
    }
    if (!overlayOptions_.showBoxes) {
        overlaySummaryLabel_->setText(QStringLiteral("Overlay hidden"));
        return;
    }
    const auto count = view_->detections().size();
    if (count == 0) {
        overlaySummaryLabel_->setText(QStringLiteral("No detections near this position"));
        return;
    }
    overlaySummaryLabel_->setText(QStringLiteral("%1 detection%2 · analysed frame %3")
                                      .arg(count)
                                      .arg(count == 1 ? QString() : QStringLiteral("s"),
                                           overlayFrameUs_ >= 0 ? timecode(overlayFrameUs_)
                                                                : kNotAvailable));
}

void ViewerPanel::closeMedia() {
    evidence_.reset();
    playback_->close();
    view_->clear(QStringLiteral("No evidence selected"));
    applyControlsEnabled(false);
    stateLabel_->setText(QStringLiteral("No media"));
    positionLabel_->setText(QStringLiteral("--:--:--.---"));
    durationLabel_->setText(QStringLiteral("--:--:--.---"));
    frameLabel_->setText(QStringLiteral("Frame —"));
}

void ViewerPanel::onFrame(const QImage& image, qint64 positionUs, qint64 frameNumber, bool) {
    view_->setFrame(image);
    if (fullScreenView_ != nullptr) fullScreenView_->setFrame(image);
    updateTimeLabels(positionUs, frameNumber);
    emit positionChanged(positionUs);
}

void ViewerPanel::updateTimeLabels(qint64 positionUs, qint64 frameNumber) {
    positionLabel_->setText(timecode(positionUs));
    if (frameNumber >= 0) {
        frameLabel_->setText(QStringLiteral("Frame %1").arg(frameNumber));
        frameLabel_->setToolTip(
            QStringLiteral("Frame index derived from the stream's constant frame rate."));
    } else {
        frameLabel_->setText(QStringLiteral("Frame n/a"));
        frameLabel_->setToolTip(QStringLiteral(
            "This stream's frame timing is variable, so a frame index would not be reliable. "
            "The presentation timestamp is authoritative."));
    }
}

void ViewerPanel::onStateChanged(int state, const QString& message) {
    const auto playbackState = static_cast<PlaybackState>(state);
    stateLabel_->setText(QString::fromUtf8(toDisplayString(playbackState)));

    switch (playbackState) {
        case PlaybackState::Error:
            view_->clear(QStringLiteral("This media could not be played.\n%1").arg(message));
            applyControlsEnabled(false);
            stateLabel_->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px;").arg(colors::kFailure.name()));
            stateLabel_->setText(QStringLiteral("Playback error — %1").arg(message));
            break;
        case PlaybackState::Ready:
        case PlaybackState::Paused:
        case PlaybackState::EndOfMedia:
            applyControlsEnabled(true);
            playButton_->setText(QStringLiteral("▶"));
            stateLabel_->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
            break;
        case PlaybackState::Playing:
            applyControlsEnabled(true);
            playButton_->setText(QStringLiteral("⏸"));
            stateLabel_->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px;").arg(colors::kAccent.name()));
            break;
        case PlaybackState::Idle:
        case PlaybackState::Opening:
            break;
    }
}

qint64 ViewerPanel::position() const { return playback_->position(); }
qint64 ViewerPanel::duration() const { return playback_->duration(); }
bool ViewerPanel::hasMedia() const { return playback_->isOpen(); }

void ViewerPanel::togglePlayPause() {
    if (!playback_->isOpen()) return;
    playback_->togglePlayPause();
}
void ViewerPanel::stepForward() {
    if (playback_->isOpen()) playback_->stepForward();
}
void ViewerPanel::stepBackward() {
    if (playback_->isOpen()) playback_->stepBackward();
}
void ViewerPanel::jumpForward() {
    if (playback_->isOpen()) playback_->jump(jumpMicroseconds_);
}
void ViewerPanel::jumpBackward() {
    if (playback_->isOpen()) playback_->jump(-jumpMicroseconds_);
}
void ViewerPanel::seek(qint64 positionUs) {
    if (playback_->isOpen()) playback_->seek(positionUs);
}

void ViewerPanel::enterFullScreen() {
    if (!playback_->isOpen()) return;
    if (fullScreenWindow_ != nullptr) {
        fullScreenWindow_->raise();
        return;
    }
    auto* window = new FullScreenWindow(this);
    fullScreenWindow_ = window;
    fullScreenView_ = window->view;
    fullScreenView_->setFrame(view_->frame());
    fullScreenView_->setOverlayOptions(overlayOptions_);
    fullScreenView_->setDetections(view_->detections());
    connect(fullScreenView_, &VideoView::detectionClicked, this,
            [this](const QString& id) { emit detectionActivated(id); });
    connect(window, &QObject::destroyed, this, [this] {
        fullScreenWindow_ = nullptr;
        fullScreenView_ = nullptr;
    });
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->showFullScreen();
}

}  // namespace trace::ui
