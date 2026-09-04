#include "ui/viewer/viewer_panel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
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

    // Audio transport is present but explicitly inactive in Phase 0: TRACE
    // decodes and reports audio metadata, but does not render audio yet, and
    // the controls say so instead of pretending.
    muteButton_ = makeTransportButton(transport, QStringLiteral("🔇"), QString());
    muteButton_->setEnabled(false);
    volumeSlider_ = new QSlider(Qt::Horizontal, transport);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(80);
    volumeSlider_->setFixedWidth(90);
    volumeSlider_->setEnabled(false);
    const QString audioTooltip =
        QStringLiteral("Audio playback is not available in Phase 0. Audio stream details are shown "
                       "in the inspector.");
    muteButton_->setToolTip(audioTooltip);
    volumeSlider_->setToolTip(audioTooltip);
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
    connect(frameExportButton_, &QPushButton::clicked, this,
            [this] { emit frameExportRequested(); });
    connect(bookmarkButton_, &QPushButton::clicked, this, [this] { emit bookmarkRequested(); });
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
        view_->clear(QStringLiteral("%1 is %2 — the Phase 0 viewer plays video only.")
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

    const double defaultSpeed =
        context_->settings().getDouble(settings_keys::kPlaybackDefaultSpeed, 1.0);
    for (int i = 0; i < speedBox_->count(); ++i) {
        if (qFuzzyCompare(speedBox_->itemData(i).toDouble(), defaultSpeed)) {
            speedBox_->setCurrentIndex(i);
            break;
        }
    }
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
    connect(window, &QObject::destroyed, this, [this] {
        fullScreenWindow_ = nullptr;
        fullScreenView_ = nullptr;
    });
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->showFullScreen();
}

}  // namespace trace::ui
