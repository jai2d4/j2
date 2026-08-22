#include "ui/app/main_window.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include "core/common/time_utils.h"
#include "trace/trace_version.h"
#include "ui/analysis/analysis_panel.h"
#include "ui/analysis/detection_inspector.h"
#include "ui/analysis/detections_panel.h"
#include "ui/annotations/annotations_panel.h"
#include "ui/annotations/bookmarks_panel.h"
#include "ui/app/application_context.h"
#include "ui/audit_viewer/audit_panel.h"
#include "ui/case_browser/case_browser_panel.h"
#include "ui/case_browser/case_dialog.h"
#include "ui/common/background_task.h"
#include "ui/common/settings_dialog.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"
#include "ui/evidence_browser/evidence_panel.h"
#include "ui/inspector/inspector_panel.h"
#include "ui/timeline/timeline_widget.h"
#include "ui/viewer/viewer_panel.h"

namespace trace::ui {
namespace {

QLabel* statusLabel(const QString& text, const QColor& colour) {
    auto* label = new QLabel(text);
    label->setStyleSheet(QStringLiteral("color: %1; padding: 0 8px;").arg(colour.name()));
    return label;
}

/// Modules that belong to later phases. They are listed so the roadmap is
/// visible, and disabled so nothing pretends to work. Object detection left this
/// list in Phase 1 — it is a real menu action now, not a placeholder.
const std::vector<QString> kFutureModules = {
    QStringLiteral("Audio analysis"), QStringLiteral("Scene understanding"),
    QStringLiteral("Entity graph"),   QStringLiteral("Reports"),
    QStringLiteral("Multi-camera")};

}  // namespace

MainWindow::MainWindow(ApplicationContext* context, QWidget* parent)
    : QMainWindow(parent), context_(context) {
    setWindowTitle(QStringLiteral("TRACE — video evidence intelligence"));
    resize(1600, 950);

    buildCentralWidgets();
    buildMenus();
    buildToolBar();
    buildStatusBar();
    connectSignals();
    updateWindowState();
    showCases();
}

MainWindow::~MainWindow() {
    for (auto* task : {frameExportTask_.get(), thumbnailTask_.get()}) {
        if (task != nullptr) {
            task->requestCancellation();
            task->wait();
        }
    }
}

void MainWindow::buildCentralWidgets() {
    stack_ = new QStackedWidget(this);
    setCentralWidget(stack_);

    caseBrowser_ = new CaseBrowserPanel(context_, stack_);
    stack_->addWidget(caseBrowser_);

    // ------------------------------------------------------- analysis layout
    analysisPage_ = new QWidget(stack_);
    auto* pageLayout = new QVBoxLayout(analysisPage_);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    auto* vertical = new QSplitter(Qt::Vertical, analysisPage_);
    auto* horizontal = new QSplitter(Qt::Horizontal, vertical);

    leftTabs_ = new QTabWidget(horizontal);
    leftTabs_->setDocumentMode(true);
    evidencePanel_ = new EvidencePanel(context_, leftTabs_);
    bookmarks_ = new BookmarksPanel(context_, leftTabs_);
    annotations_ = new AnnotationsPanel(context_, leftTabs_);
    detections_ = new DetectionsPanel(context_, leftTabs_);
    leftTabs_->addTab(evidencePanel_, QStringLiteral("Evidence"));
    leftTabs_->addTab(bookmarks_, QStringLiteral("Bookmarks"));
    leftTabs_->addTab(annotations_, QStringLiteral("Notes"));
    leftTabs_->addTab(detections_, QStringLiteral("Detections"));

    viewer_ = new ViewerPanel(context_, horizontal);

    rightTabs_ = new QTabWidget(horizontal);
    rightTabs_->setDocumentMode(true);
    inspector_ = new InspectorPanel(context_, rightTabs_);
    analysis_ = new AnalysisPanel(context_, rightTabs_);
    detectionInspector_ = new DetectionInspector(rightTabs_);
    rightTabs_->addTab(inspector_, QStringLiteral("Evidence"));
    rightTabs_->addTab(analysis_, QStringLiteral("AI analysis"));
    rightTabs_->addTab(detectionInspector_, QStringLiteral("Detection"));

    horizontal->addWidget(leftTabs_);
    horizontal->addWidget(viewer_);
    horizontal->addWidget(rightTabs_);
    horizontal->setStretchFactor(0, 0);
    horizontal->setStretchFactor(1, 1);
    horizontal->setStretchFactor(2, 0);
    horizontal->setSizes({330, 880, 400});

    timeline_ = new TimelineWidget(vertical);
    vertical->addWidget(horizontal);
    vertical->addWidget(timeline_);
    vertical->setStretchFactor(0, 1);
    vertical->setStretchFactor(1, 0);
    vertical->setSizes({720, 130});

    pageLayout->addWidget(vertical);
    stack_->addWidget(analysisPage_);

    audit_ = new AuditPanel(context_, stack_);
    stack_->addWidget(audit_);
}

void MainWindow::buildMenus() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* newCase = fileMenu->addAction(QStringLiteral("&New case…"));
    newCase->setShortcut(QKeySequence::New);
    connect(newCase, &QAction::triggered, this, [this] {
        showCases();
        CaseDialog dialog(context_, CaseDialog::Mode::Create, std::nullopt, this);
        if (dialog.exec() != QDialog::Accepted || !dialog.result()) return;
        caseBrowser_->refresh();
        if (auto status = context_->openCase(QString::fromStdString(dialog.result()->id)); status) {
            showViewer();
        }
    });

    auto* openCases = fileMenu->addAction(QStringLiteral("&Case list"));
    connect(openCases, &QAction::triggered, this, &MainWindow::showCases);

    fileMenu->addSeparator();
    importAction_ = fileMenu->addAction(QStringLiteral("&Import evidence…"));
    importAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    connect(importAction_, &QAction::triggered, this,
            [this] { evidencePanel_->importEvidence(); });

    editCaseAction_ = fileMenu->addAction(QStringLiteral("Edit case &details…"));
    connect(editCaseAction_, &QAction::triggered, this, [this] {
        if (!context_->currentCase()) return;
        CaseDialog dialog(context_, CaseDialog::Mode::Edit, context_->currentCase(), this);
        if (dialog.exec() != QDialog::Accepted) return;
        context_->refreshCurrentCase();
        context_->notifyCasesChanged();
        updateWindowState();
    });

    fileMenu->addSeparator();
    auto* settings = fileMenu->addAction(QStringLiteral("&Settings…"));
    connect(settings, &QAction::triggered, this, [this] {
        SettingsDialog dialog(context_, this);
        dialog.exec();
    });

    fileMenu->addSeparator();
    auto* quit = fileMenu->addAction(QStringLiteral("E&xit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    // ------------------------------------------------------------- evidence
    auto* evidenceMenu = menuBar()->addMenu(QStringLiteral("&Evidence"));
    verifyAction_ = evidenceMenu->addAction(QStringLiteral("&Verify integrity"));
    verifyAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    connect(verifyAction_, &QAction::triggered, this, [this] { inspector_->verifyIntegrity(); });

    saveFrameAction_ = evidenceMenu->addAction(QStringLiteral("&Save current frame"));
    saveFrameAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(saveFrameAction_, &QAction::triggered, this, &MainWindow::saveCurrentFrame);

    evidenceMenu->addSeparator();
    bookmarkAction_ = evidenceMenu->addAction(QStringLiteral("Add &bookmark at playhead"));
    bookmarkAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    connect(bookmarkAction_, &QAction::triggered, this, &MainWindow::addBookmarkAtPlayhead);

    annotationAction_ = evidenceMenu->addAction(QStringLiteral("Add &note at playhead"));
    annotationAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    connect(annotationAction_, &QAction::triggered, this, &MainWindow::addAnnotationAtPlayhead);

    // ----------------------------------------------------------------- view
    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    casesAction_ = viewMenu->addAction(QStringLiteral("&Cases"));
    connect(casesAction_, &QAction::triggered, this, &MainWindow::showCases);
    viewerAction_ = viewMenu->addAction(QStringLiteral("&Viewer and timeline"));
    connect(viewerAction_, &QAction::triggered, this, &MainWindow::showViewer);
    auditAction_ = viewMenu->addAction(QStringLiteral("&Audit trail"));
    connect(auditAction_, &QAction::triggered, this, &MainWindow::showAudit);

    viewMenu->addSeparator();
    auto* fullScreen = viewMenu->addAction(QStringLiteral("&Full screen viewer"));
    fullScreen->setShortcut(QKeySequence(QStringLiteral("F11")));
    connect(fullScreen, &QAction::triggered, this, [this] { viewer_->enterFullScreen(); });

    viewMenu->addSeparator();
    auto* zoomIn = viewMenu->addAction(QStringLiteral("Timeline zoom &in"));
    zoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(zoomIn, &QAction::triggered, this, [this] { timeline_->zoomIn(); });
    auto* zoomOut = viewMenu->addAction(QStringLiteral("Timeline zoom &out"));
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, this, [this] { timeline_->zoomOut(); });
    auto* zoomReset = viewMenu->addAction(QStringLiteral("Timeline &fit"));
    connect(zoomReset, &QAction::triggered, this, [this] { timeline_->resetZoom(); });

    // ----------------------------------------------------------- AI analysis
    auto* analysisMenu = menuBar()->addMenu(QStringLiteral("&Analysis"));
    analyzeAction_ = analysisMenu->addAction(QStringLiteral("&Analyze video…"));
    analyzeAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    analyzeAction_->setToolTip(
        QStringLiteral("Run object detection over this item. The managed original is read, never "
                       "written."));
    connect(analyzeAction_, &QAction::triggered, this, &MainWindow::analyzeCurrentEvidence);

    cancelAnalysisAction_ = analysisMenu->addAction(QStringLiteral("&Cancel analysis"));
    cancelAnalysisAction_->setEnabled(false);
    connect(cancelAnalysisAction_, &QAction::triggered, this,
            [this] { analysis_->cancelAnalysis(); });

    analysisMenu->addSeparator();
    auto* detectionsAction = analysisMenu->addAction(QStringLiteral("&Detections list"));
    connect(detectionsAction, &QAction::triggered, this, &MainWindow::showDetections);

    // -------------------------------------------------------------- modules
    auto* moduleMenu = menuBar()->addMenu(QStringLiteral("&Modules"));
    auto* phaseNote = moduleMenu->addAction(QStringLiteral("Not built yet — later phases"));
    phaseNote->setEnabled(false);
    moduleMenu->addSeparator();
    for (const auto& name : kFutureModules) {
        auto* action = moduleMenu->addAction(name);
        action->setEnabled(false);
        action->setToolTip(QStringLiteral("Planned for a later phase — not implemented"));
    }

    auto* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    auto* about = helpMenu->addAction(QStringLiteral("&About TRACE"));
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::buildToolBar() {
    auto* toolBar = addToolBar(QStringLiteral("Main"));
    toolBar->setMovable(false);
    toolBar->setIconSize(QSize(16, 16));

    toolBar->addAction(casesAction_);
    toolBar->addAction(viewerAction_);
    toolBar->addAction(auditAction_);
    toolBar->addSeparator();
    toolBar->addAction(importAction_);
    toolBar->addAction(verifyAction_);
    toolBar->addAction(saveFrameAction_);
    toolBar->addSeparator();
    toolBar->addAction(bookmarkAction_);
    toolBar->addAction(annotationAction_);
    toolBar->addSeparator();
    toolBar->addAction(analyzeAction_);
    toolBar->addAction(cancelAnalysisAction_);
}

void MainWindow::buildStatusBar() {
    caseStatusLabel_ = statusLabel(QStringLiteral("No case open"), colors::kTextSecondary);
    evidenceStatusLabel_ = statusLabel(QString(), colors::kTextSecondary);
    integrityStatusLabel_ = statusLabel(QString(), colors::kTextSecondary);
    positionStatusLabel_ = statusLabel(QString(), colors::kTextSecondary);
    positionStatusLabel_->setFont(monospaceFont(-1));

    // All of these are permanent: Qt hides ordinary status-bar widgets while a
    // temporary message is showing, which would make the case identity vanish
    // exactly when the operator is reading a message about it.
    statusBar()->addPermanentWidget(caseStatusLabel_, 1);
    statusBar()->addPermanentWidget(evidenceStatusLabel_);
    statusBar()->addPermanentWidget(integrityStatusLabel_);
    statusBar()->addPermanentWidget(positionStatusLabel_);
    statusBar()->showMessage(
        QStringLiteral("TRACE %1 — %2").arg(QString::fromUtf8(kApplicationVersion),
                                            QString::fromUtf8(kPhase)),
        6000);
}

void MainWindow::connectSignals() {
    connect(caseBrowser_, &CaseBrowserPanel::caseOpened, this, [this] {
        showViewer();
        updateWindowState();
    });

    connect(evidencePanel_, &EvidencePanel::evidenceActivated, this,
            &MainWindow::openEvidenceInViewer);

    connect(viewer_, &ViewerPanel::positionChanged, this, [this](qint64 positionUs) {
        timeline_->setPosition(positionUs);
        positionStatusLabel_->setText(timecode(positionUs));
        updateDetectionOverlay();
    });
    connect(viewer_, &ViewerPanel::durationChanged, this, [this](qint64 durationUs) {
        timeline_->setDuration(durationUs);
        refreshTimelineTracks();
    });
    connect(viewer_, &ViewerPanel::bookmarkRequested, this, &MainWindow::addBookmarkAtPlayhead);
    connect(viewer_, &ViewerPanel::annotationRequested, this, &MainWindow::addAnnotationAtPlayhead);
    connect(viewer_, &ViewerPanel::frameExportRequested, this, &MainWindow::saveCurrentFrame);
    connect(viewer_->playback(), &PlaybackBridge::frameReady, this,
            [this](const QImage&, qint64, qint64 frameNumber, bool) {
                lastFrameNumber_ = frameNumber;
            });

    connect(timeline_, &TimelineWidget::seekRequested, this,
            [this](qint64 positionUs) { viewer_->seek(positionUs); });
    connect(timeline_, &TimelineWidget::markerActivated, this,
            [this](const QString& track, const QString& id) {
                if (track == QStringLiteral("Bookmarks")) {
                    leftTabs_->setCurrentWidget(bookmarks_);
                    bookmarks_->selectBookmark(id);
                    return;
                }
                // Detection lane markers carry "<group>:<start µs>": activating
                // one moves the playhead to where that presence begins and
                // selects the nearest detection in the list.
                const qsizetype separator = id.lastIndexOf(QLatin1Char(':'));
                if (separator < 0) return;
                bool parsed = false;
                const qint64 startUs = id.mid(separator + 1).toLongLong(&parsed);
                if (!parsed) return;
                viewer_->seek(startUs);
                leftTabs_->setCurrentWidget(detections_);
                detections_->selectNearestTo(startUs);
            });

    connect(bookmarks_, &BookmarksPanel::jumpRequested, this,
            [this](qint64 positionUs) { viewer_->seek(positionUs); });
    connect(bookmarks_, &BookmarksPanel::addRequested, this, &MainWindow::addBookmarkAtPlayhead);
    connect(bookmarks_, &BookmarksPanel::bookmarksChanged, this,
            &MainWindow::refreshTimelineTracks);
    connect(annotations_, &AnnotationsPanel::jumpRequested, this,
            [this](qint64 positionUs) { viewer_->seek(positionUs); });
    connect(annotations_, &AnnotationsPanel::addRequested, this,
            &MainWindow::addAnnotationAtPlayhead);
    connect(annotations_, &AnnotationsPanel::annotationsChanged, this,
            &MainWindow::refreshTimelineTracks);

    connect(inspector_, &InspectorPanel::statusMessage, this,
            [this](const QString& message, int timeout) {
                statusBar()->showMessage(message.split(QLatin1Char('\n')).first(), timeout);
            });
    connect(inspector_, &InspectorPanel::integrityChecked, this,
            [this](bool) { updateStatusForEvidence(); });

    // --------------------------------------------------------- AI analysis
    connect(analysis_, &AnalysisPanel::statusMessage, this,
            [this](const QString& message, int timeout) {
                statusBar()->showMessage(message, timeout);
            });
    connect(analysis_, &AnalysisPanel::analysisStarted, this, [this] {
        cancelAnalysisAction_->setEnabled(true);
        analyzeAction_->setEnabled(false);
    });
    connect(analysis_, &AnalysisPanel::progressUpdated, this,
            [this](qint64 frames, qint64 found, double fraction) {
                statusBar()->showMessage(QStringLiteral("Analysing — %1% · %2 frames · %3 "
                                                        "detections")
                                             .arg(fraction * 100.0, 0, 'f', 0)
                                             .arg(frames)
                                             .arg(found),
                                         0);
            });
    connect(analysis_, &AnalysisPanel::analysisFinished, this,
            [this](bool, const QString& message, int terminalStatus) {
                cancelAnalysisAction_->setEnabled(false);
                updateWindowState();
                // Cancelling is a decision the operator just made — reporting it
                // back as a problem would be noise. A failure is different, and
                // is put in front of them.
                const auto status = static_cast<AnalysisRunStatus>(terminalStatus);
                if (status == AnalysisRunStatus::Failed ||
                    status == AnalysisRunStatus::Partial) {
                    QMessageBox::warning(this, QStringLiteral("Analysis did not complete"),
                                         message);
                }
            });
    connect(analysis_, &AnalysisPanel::resultsRunChanged, this, [this] {
        detections_->setResultsRun(analysis_->resultsRun());
        detectionInspector_->clear();
        viewer_->setDetectionOverlayAvailable(analysis_->resultsRun().has_value());
        refreshTimelineTracks();
        updateDetectionOverlay();
    });

    connect(detections_, &DetectionsPanel::statusMessage, this,
            [this](const QString& message, int timeout) {
                statusBar()->showMessage(message, timeout);
            });
    connect(detections_, &DetectionsPanel::jumpRequested, this,
            [this](qint64 positionUs) { viewer_->seek(positionUs); });
    connect(detections_, &DetectionsPanel::detectionSelected, this,
            [this](const QString& detectionId) {
                viewer_->setSelectedDetection(detectionId);
                showDetectionInInspector(detectionId);
            });
    connect(detections_, &DetectionsPanel::filtersChanged, this, [this] {
        refreshTimelineTracks();
        updateDetectionOverlay();
    });
    connect(detections_, &DetectionsPanel::verificationChanged, this, [this] {
        refreshTimelineTracks();
        updateDetectionOverlay();
        showDetectionInInspector(detections_->selectedDetectionId());
    });

    connect(viewer_, &ViewerPanel::detectionActivated, this, [this](const QString& detectionId) {
        if (detectionId.isEmpty()) return;
        leftTabs_->setCurrentWidget(detections_);
        detections_->selectDetection(detectionId);
        rightTabs_->setCurrentWidget(detectionInspector_);
    });
    connect(viewer_, &ViewerPanel::overlayOptionsChanged, this,
            [this](const DetectionOverlayOptions&) { updateDetectionOverlay(); });

    connect(context_, &ApplicationContext::currentEvidenceChanged, this,
            &MainWindow::updateStatusForEvidence);
    connect(context_, &ApplicationContext::caseOpened, this, &MainWindow::updateWindowState);
    connect(context_, &ApplicationContext::currentCaseUpdated, this, &MainWindow::updateWindowState);
    connect(context_, &ApplicationContext::evidenceChanged, this,
            [this] { updateWindowState(); });

    // Viewer keyboard control, active whenever the analysis workspace is shown.
    const auto addShortcut = [this](const QKeySequence& sequence, auto&& handler) {
        auto* shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this, handler);
    };
    addShortcut(QKeySequence(Qt::Key_Space), [this] { viewer_->togglePlayPause(); });
    addShortcut(QKeySequence(Qt::Key_Left), [this] { viewer_->stepBackward(); });
    addShortcut(QKeySequence(Qt::Key_Right), [this] { viewer_->stepForward(); });
    addShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Left), [this] { viewer_->jumpBackward(); });
    addShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Right), [this] { viewer_->jumpForward(); });
    addShortcut(QKeySequence(Qt::Key_Home), [this] { viewer_->seek(0); });
}

void MainWindow::showCases() {
    stack_->setCurrentWidget(caseBrowser_);
    caseBrowser_->refresh();
}

void MainWindow::showViewer() {
    stack_->setCurrentWidget(analysisPage_);
    updateWindowState();
}

void MainWindow::showAudit() {
    stack_->setCurrentWidget(audit_);
    audit_->refresh();
}

void MainWindow::openEvidenceInViewer(const Evidence& evidence) {
    showViewer();
    viewer_->openEvidence(evidence);
    context_->evidence().recordViewed(evidence, context_->currentCaseNumber().toStdString());
    context_->notifyAuditChanged();
    timeline_->setDuration(0);
    // The analysis panel picks the run to view and tells the detections panel,
    // which in turn feeds the overlay and the timeline lanes.
    analysis_->setEvidence(evidence);
    detections_->setEvidence(evidence);
    detections_->setResultsRun(analysis_->resultsRun());
    detectionInspector_->clear();
    viewer_->setDetectionOverlayAvailable(analysis_->resultsRun().has_value());
    refreshTimelineTracks();
    updateStatusForEvidence();

    // Generate the preview image once per item, off the GUI thread. It is a
    // derived asset like any other, with its own provenance record.
    if (evidence.mediaType == MediaType::Video && thumbnailTask_ == nullptr) {
        thumbnailTask_ = std::make_unique<BackgroundTask>();
        auto* context = context_;
        const QString caseNumber = context_->currentCaseNumber();
        connect(thumbnailTask_.get(), &BackgroundTask::finished, this, [this](bool ok, QString) {
            if (thumbnailTask_ != nullptr) {
                thumbnailTask_->wait();
                thumbnailTask_.reset();
            }
            if (ok) {
                context_->notifyDerivedAssetsChanged();
                evidencePanel_->refresh();
            }
        });
        thumbnailTask_->start([context, evidence, caseNumber](BackgroundTask& task) {
            auto result = context->frameExports().ensureThumbnail(
                evidence.caseId, caseNumber.toStdString(), evidence);
            task.reportFinished(static_cast<bool>(result), QString());
        });
    }
}

void MainWindow::refreshTimelineTracks() {
    if (!context_->isInitialised()) return;

    std::vector<TimelineTrack> tracks;

    TimelineTrack bookmarkTrack;
    bookmarkTrack.name = QStringLiteral("Bookmarks");
    bookmarkTrack.colour = colors::kBookmark;
    for (const auto& bookmark : bookmarks_->bookmarks()) {
        TimelineMarker marker;
        marker.id = QString::fromStdString(bookmark.id);
        marker.startUs = bookmark.timestampUs;
        marker.label = QString::fromStdString(bookmark.title);
        marker.colour = colors::kBookmark;
        bookmarkTrack.markers.push_back(std::move(marker));
    }
    tracks.push_back(std::move(bookmarkTrack));

    TimelineTrack annotationTrack;
    annotationTrack.name = QStringLiteral("Notes");
    annotationTrack.colour = colors::kAnnotation;
    for (const auto& annotation : annotations_->annotations()) {
        TimelineMarker marker;
        marker.id = QString::fromStdString(annotation.id);
        marker.startUs = annotation.startUs;
        if (annotation.endUs) marker.endUs = *annotation.endUs;
        marker.label = QString::fromStdString(annotation.text);
        marker.colour = colors::kAnnotation;
        annotationTrack.markers.push_back(std::move(marker));
    }
    tracks.push_back(std::move(annotationTrack));

    // Detection lanes come last so the analyst's own marks stay at the top.
    for (auto& lane : detections_->timelineTracks()) tracks.push_back(std::move(lane));

    timeline_->setTracks(std::move(tracks));
}

void MainWindow::updateDetectionOverlay() {
    if (!context_->isInitialised()) return;
    if (!viewer_->detectionOverlayAvailable() || !viewer_->overlayOptions().showBoxes) {
        viewer_->setDetections({});
        return;
    }
    const DetectionOverlayFrame frame = detections_->overlayAt(viewer_->position());
    viewer_->setDetections(frame.items, frame.timestampUs);
}

void MainWindow::showDetectionInInspector(const QString& detectionId) {
    if (detectionId.isEmpty() || !context_->isInitialised()) {
        detectionInspector_->clear();
        return;
    }
    auto found = context_->analysis().findDetection(detectionId.toStdString());
    if (!found || !found.value().has_value()) {
        detectionInspector_->clear();
        return;
    }
    detectionInspector_->showDetection(found.value(), analysis_->resultsRun(),
                                       context_->currentEvidence());
}

void MainWindow::analyzeCurrentEvidence() {
    showViewer();
    rightTabs_->setCurrentWidget(analysis_);
    analysis_->startAnalysis();
}

void MainWindow::showDetections() {
    showViewer();
    leftTabs_->setCurrentWidget(detections_);
}

void MainWindow::addBookmarkAtPlayhead() {
    if (!context_->currentEvidence()) {
        statusBar()->showMessage(QStringLiteral("Open an evidence item before adding a bookmark."),
                                 5000);
        return;
    }
    leftTabs_->setCurrentWidget(bookmarks_);
    bookmarks_->addBookmarkAt(viewer_->position(), lastFrameNumber_);
    refreshTimelineTracks();
}

void MainWindow::addAnnotationAtPlayhead() {
    if (!context_->currentEvidence()) {
        statusBar()->showMessage(QStringLiteral("Open an evidence item before adding a note."), 5000);
        return;
    }
    leftTabs_->setCurrentWidget(annotations_);
    annotations_->addAnnotationAt(viewer_->position(), lastFrameNumber_);
    refreshTimelineTracks();
}

void MainWindow::saveCurrentFrame() {
    if (!context_->currentEvidence() || !context_->currentCase()) return;
    if (frameExportTask_ != nullptr) return;

    auto frame = viewer_->playback()->currentFrame();
    if (!frame || !frame->valid()) {
        statusBar()->showMessage(QStringLiteral("There is no decoded frame to save yet."), 5000);
        return;
    }

    FrameExportService::ExportRequest request;
    request.caseId = context_->currentCase()->id;
    request.caseNumber = context_->currentCase()->caseNumber;
    request.evidence = *context_->currentEvidence();
    request.decoderInfo = viewer_->playback()->streamInfo();

    statusBar()->showMessage(QStringLiteral("Saving frame at %1…").arg(timecode(frame->presentationUs)),
                             0);
    frameExportTask_ = std::make_unique<BackgroundTask>();
    auto* context = context_;

    connect(frameExportTask_.get(), &BackgroundTask::finished, this,
            [this](bool succeeded, const QString& message) {
                if (frameExportTask_ != nullptr) {
                    frameExportTask_->wait();
                    frameExportTask_.reset();
                }
                statusBar()->showMessage(message, 8000);
                context_->notifyDerivedAssetsChanged();
                context_->notifyAuditChanged();
                if (!succeeded) {
                    QMessageBox::warning(this, QStringLiteral("Frame not saved"), message);
                }
            });

    frameExportTask_->start([context, request, frame](BackgroundTask& task) mutable {
        request.frame = frame.get();
        auto exported = context->frameExports().exportFrame(request);
        if (!exported) {
            task.reportFinished(false, QStringLiteral("Frame not saved: %1")
                                           .arg(QString::fromStdString(exported.error().toString())));
            return;
        }
        task.reportFinished(true, QStringLiteral("Frame saved as %1 (derived asset, SHA-256 %2…)")
                                      .arg(QString::fromStdString(exported.value().filename),
                                           QString::fromStdString(
                                               exported.value().sha256.value_or("").substr(0, 12))));
    });
}

void MainWindow::updateWindowState() {
    const bool hasCase = context_->currentCase().has_value();
    const bool hasEvidence = context_->currentEvidence().has_value();

    importAction_->setEnabled(hasCase);
    editCaseAction_->setEnabled(hasCase);
    verifyAction_->setEnabled(hasEvidence);
    saveFrameAction_->setEnabled(hasEvidence);
    bookmarkAction_->setEnabled(hasEvidence);
    annotationAction_->setEnabled(hasEvidence);
    viewerAction_->setEnabled(hasCase);

    const bool analysisRunning = analysis_ != nullptr && analysis_->analysisRunning();
    const bool isVideo = hasEvidence && context_->currentEvidence()->mediaType == MediaType::Video;
    if (analyzeAction_ != nullptr) analyzeAction_->setEnabled(isVideo && !analysisRunning);
    if (cancelAnalysisAction_ != nullptr) cancelAnalysisAction_->setEnabled(analysisRunning);

    if (hasCase) {
        const Case& value = *context_->currentCase();
        caseStatusLabel_->setText(QStringLiteral("%1  ·  %2  ·  %3")
                                      .arg(QString::fromStdString(value.caseNumber),
                                           QString::fromStdString(value.title),
                                           QString::fromUtf8(toDisplayString(value.status))));
        setWindowTitle(QStringLiteral("TRACE — %1 %2")
                           .arg(QString::fromStdString(value.caseNumber),
                                QString::fromStdString(value.title)));
    } else {
        caseStatusLabel_->setText(QStringLiteral("No case open"));
        setWindowTitle(QStringLiteral("TRACE — video evidence intelligence"));
    }
    updateStatusForEvidence();
}

void MainWindow::updateStatusForEvidence() {
    const auto& evidence = context_->currentEvidence();
    if (!evidence) {
        evidenceStatusLabel_->setText(QString());
        integrityStatusLabel_->setText(QString());
        positionStatusLabel_->setText(QString());
        return;
    }
    evidenceStatusLabel_->setText(QStringLiteral("%1  %2")
                                      .arg(QString::fromStdString(evidence->evidenceNumber),
                                           QString::fromStdString(evidence->originalFilename)));
    integrityStatusLabel_->setText(QString::fromUtf8(toDisplayString(evidence->integrityStatus)));
    integrityStatusLabel_->setStyleSheet(
        QStringLiteral("color: %1; padding: 0 8px; font-weight: 600;")
            .arg(integrityColour(evidence->integrityStatus).name()));
}

void MainWindow::showAbout() {
    QMessageBox::about(
        this, QStringLiteral("About TRACE"),
        QStringLiteral(
            "<h3>TRACE %1</h3>"
            "<p>%2 — video evidence intelligence platform.</p>"
            "<p>Original evidence is copied into managed storage, hashed with SHA-256 and never "
            "modified. Everything produced from it is recorded as a derived asset with its own "
            "provenance, and every action is written to an append-only audit trail.</p>"
            "<p>Object detection reads the managed original and stores what a model reported, "
            "with the digest of the exact model file that produced it. Detection overlays are "
            "drawn on screen only — the evidence file is never altered.</p>"
            "<p><b>TRACE makes no forensic determinations.</b> It reports a visual class and a "
            "confidence: it does not identify individuals, read number plates, or say what was "
            "happening. Conclusions remain the analyst's.</p>"
            "<p style='color:#8d99a4'>Qt %3 · SQLite · FFmpeg</p>")
            .arg(QString::fromUtf8(kApplicationVersion), QString::fromUtf8(kPhase),
                 QString::fromUtf8(qVersion())));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    viewer_->closeMedia();

    // An analysis in flight writes to the database, so it has to stop before the
    // services it is writing through are released. Cancelling keeps whatever it
    // has already stored and records the run as cancelled.
    if (analysis_ != nullptr) analysis_->cancelAnalysis();

    // Stop background work and then destroy the task objects. A finished
    // signal may already be queued for delivery; deleting its sender discards
    // it, which is what keeps a completing export from calling into services
    // that shutdown() is about to release.
    for (auto* task : {frameExportTask_.get(), thumbnailTask_.get()}) {
        if (task != nullptr) {
            task->requestCancellation();
            task->wait();
        }
    }
    frameExportTask_.reset();
    thumbnailTask_.reset();

    context_->shutdown();
    QMainWindow::closeEvent(event);
}

}  // namespace trace::ui
