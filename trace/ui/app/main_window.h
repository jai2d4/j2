#pragma once

#include <QMainWindow>

#include <memory>
#include <optional>
#include <string>

#include "core/models/evidence.h"
#include "media/audio/waveform.h"

class QAction;
class QLabel;
class QSplitter;
class QStackedWidget;
class QTabWidget;

namespace trace::ui {

class AnalysisPanel;
class AnnotationsPanel;
class ApplicationContext;
class AuditPanel;
class BackgroundTask;
class BookmarksPanel;
class CaseBrowserPanel;
class DetectionInspector;
class DetectionsPanel;
class EvidencePanel;
class InspectorPanel;
class ReportBuilderDialog;
class TimelineWidget;
class ViewerPanel;

/// The TRACE main window.
///
///   ┌─────────────────────────────────────────────────────────┐
///   │ TRACE                                                   │
///   ├──────────────┬──────────────────────────────┬───────────┤
///   │ EVIDENCE     │                              │ INSPECTOR │
///   │ BOOKMARKS    │   VIDEO VIEWER + AI OVERLAY  │ AI RUN    │
///   │ NOTES        │                              │ DETECTION │
///   │ DETECTIONS   │                              │           │
///   ├──────────────┴──────────────────────────────┴───────────┤
///   │        TIMELINE — bookmarks, notes, detection lanes     │
///   └─────────────────────────────────────────────────────────┘
///
/// The window owns no domain logic: it routes user intent to services through
/// ApplicationContext and keeps the panels in step with each other.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(ApplicationContext* context, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Navigation targets, also used by the automated acceptance test.
    void showCases();
    void showViewer();
    void showAudit();

    // Accessors used by the acceptance test to drive the real UI.
    CaseBrowserPanel* caseBrowser() const { return caseBrowser_; }
    EvidencePanel* evidencePanel() const { return evidencePanel_; }
    ViewerPanel* viewerPanel() const { return viewer_; }
    InspectorPanel* inspectorPanel() const { return inspector_; }
    BookmarksPanel* bookmarksPanel() const { return bookmarks_; }
    AnnotationsPanel* annotationsPanel() const { return annotations_; }
    TimelineWidget* timeline() const { return timeline_; }
    AuditPanel* auditPanel() const { return audit_; }
    AnalysisPanel* analysisPanel() const { return analysis_; }
    DetectionsPanel* detectionsPanel() const { return detections_; }
    DetectionInspector* detectionInspector() const { return detectionInspector_; }

    /// Brings the AI analysis panel forward and starts a run — the Analyze
    /// Video action, also used by the acceptance test.
    void analyzeCurrentEvidence();
    void showDetections();
    /// Opens the exhibit bundle builder for the current case.
    ReportBuilderDialog* exportExhibitBundle();
    void verifyExhibitBundle();

    void openEvidenceInViewer(const Evidence& evidence);
    void addBookmarkAtPlayhead();
    void addAnnotationAtPlayhead();
    void saveCurrentFrame();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildMenus();
    void buildToolBar();
    void buildStatusBar();
    void buildCentralWidgets();
    void connectSignals();
    void refreshTimelineTracks();
    void encryptWorkspace();
    void updateDetectionOverlay();
    void showDetectionInInspector(const QString& detectionId);
    void updateWindowState();
    void updateStatusForEvidence();
    void showAbout();

    ApplicationContext* context_ = nullptr;

    QStackedWidget* stack_ = nullptr;
    CaseBrowserPanel* caseBrowser_ = nullptr;
    QWidget* analysisPage_ = nullptr;
    QTabWidget* leftTabs_ = nullptr;
    EvidencePanel* evidencePanel_ = nullptr;
    BookmarksPanel* bookmarks_ = nullptr;
    AnnotationsPanel* annotations_ = nullptr;
    DetectionsPanel* detections_ = nullptr;
    ViewerPanel* viewer_ = nullptr;
    QTabWidget* rightTabs_ = nullptr;
    InspectorPanel* inspector_ = nullptr;
    AnalysisPanel* analysis_ = nullptr;
    DetectionInspector* detectionInspector_ = nullptr;
    TimelineWidget* timeline_ = nullptr;
    AuditPanel* audit_ = nullptr;

    QLabel* caseStatusLabel_ = nullptr;
    QLabel* evidenceStatusLabel_ = nullptr;
    QLabel* integrityStatusLabel_ = nullptr;
    QLabel* positionStatusLabel_ = nullptr;

    QAction* importAction_ = nullptr;
    QAction* verifyAction_ = nullptr;
    QAction* saveFrameAction_ = nullptr;
    QAction* bookmarkAction_ = nullptr;
    QAction* annotationAction_ = nullptr;
    QAction* editCaseAction_ = nullptr;
    QAction* analyzeAction_ = nullptr;
    QAction* cancelAnalysisAction_ = nullptr;
    QAction* exportBundleAction_ = nullptr;
    QAction* verifyBundleAction_ = nullptr;
    QAction* casesAction_ = nullptr;
    QAction* viewerAction_ = nullptr;
    QAction* auditAction_ = nullptr;

    std::unique_ptr<BackgroundTask> frameExportTask_;
    std::unique_ptr<BackgroundTask> thumbnailTask_;
    QAction* encryptWorkspaceAction_ = nullptr;
    std::unique_ptr<BackgroundTask> waveformTask_;
    /// Envelope for the item in the viewer, once it has been built.
    std::optional<Waveform> waveform_;
    std::string waveformEvidenceId_;
    qint64 lastFrameNumber_ = -1;
};

}  // namespace trace::ui
