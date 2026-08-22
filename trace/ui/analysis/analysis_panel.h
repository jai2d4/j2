#pragma once

#include <QWidget>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "analysis/analysis_pipeline.h"
#include "core/models/analysis_run.h"
#include "core/models/evidence.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace trace::ui {

class ApplicationContext;
class BackgroundTask;

/// Configures and runs object detection over the evidence in the viewer, and
/// lists what has been run against it before.
///
/// The analysis itself happens on a worker thread; this panel only configures
/// it, shows live progress and reports the outcome. Nothing here writes to the
/// evidence file: a run reads the managed original and writes rows to the
/// database, and the original's digest is recorded on the run so the results can
/// always be tied back to the exact bytes that produced them.
class AnalysisPanel : public QWidget {
    Q_OBJECT

public:
    explicit AnalysisPanel(ApplicationContext* context, QWidget* parent = nullptr);
    ~AnalysisPanel() override;

    void setEvidence(const std::optional<Evidence>& evidence);
    /// Re-reads the run history and re-selects the run being viewed.
    void refresh();

    bool analysisRunning() const;
    /// The Analyze Video action. Safe to call when nothing is selected: it says
    /// why it cannot start rather than doing nothing.
    void startAnalysis();
    void cancelAnalysis();

    /// The run whose results the rest of the workspace is showing.
    const std::optional<AnalysisRun>& resultsRun() const { return resultsRun_; }
    QString resultsRunId() const;

    // Used by the acceptance tests to drive the real controls.
    QPushButton* analyzeButton() const { return analyzeButton_; }
    QPushButton* cancelButton() const { return cancelButton_; }
    QComboBox* providerBox() const { return providerBox_; }
    QComboBox* modelBox() const { return modelBox_; }
    QComboBox* qualityBox() const { return qualityBox_; }
    QComboBox* deviceBox() const { return deviceBox_; }
    QDoubleSpinBox* confidenceBox() const { return confidenceBox_; }
    QTreeWidget* runTree() const { return runTree_; }

signals:
    void statusMessage(const QString& message, int timeoutMs);
    void analysisStarted();
    /// Live progress, already throttled to the panel's refresh rate. Carries the
    /// counters a status bar wants without every listener having to poll.
    void progressUpdated(qint64 framesAnalyzed, qint64 detectionsFound, double fractionComplete);
    /// Emitted once per run, whatever the outcome. `terminalStatus` is the
    /// recorded AnalysisRunStatus as an int, so a listener can tell a run the
    /// operator cancelled from one that actually failed — both are "not
    /// completed", but only one of them is a problem to report.
    void analysisFinished(bool succeeded, const QString& message, int terminalStatus);
    /// The run being viewed changed — the overlay, timeline lanes and detections
    /// table all follow this.
    void resultsRunChanged();

private:
    void buildUi();
    void reloadProviders();
    void reloadModels();
    void updateModelStatus();
    void updateControlState();
    void reloadRuns();
    void selectRunInTree(const QString& runId);
    void applyProgressToUi();
    void showIdleProgress(const QString& message);
    DetectionAnalysisRequest buildRequest() const;

    ApplicationContext* context_ = nullptr;
    std::optional<Evidence> evidence_;
    std::vector<AnalysisRun> runs_;
    std::optional<AnalysisRun> resultsRun_;

    std::unique_ptr<BackgroundTask> task_;
    /// True from the moment a run is committed to until its outcome has been
    /// handled. Deliberately not derived from the worker thread's own state:
    /// the controls must switch the instant the operator asks for a run, not
    /// once the thread happens to be up.
    bool runInFlight_ = false;
    /// Which item the running analysis belongs to. The operator may open
    /// another item while a run is in flight; the run keeps its own copy of the
    /// request, and the progress display keeps saying what it is working on.
    QString runningEvidenceNumber_;
    bool providerRequiresModel_ = true;
    std::shared_ptr<std::atomic<bool>> cancellationToken_;
    QTimer* progressTimer_ = nullptr;

    /// Written by the analysis thread, read by the GUI thread on a timer. The
    /// pipeline reports every frame; repainting at that rate is the UI's problem
    /// to solve, and this is where it is solved.
    mutable std::mutex progressMutex_;
    AnalysisProgress latestProgress_;
    std::optional<AnalysisOutcome> lastOutcome_;

    QLabel* evidenceLabel_ = nullptr;
    QComboBox* providerBox_ = nullptr;
    QComboBox* modelBox_ = nullptr;
    QComboBox* qualityBox_ = nullptr;
    QComboBox* deviceBox_ = nullptr;
    QDoubleSpinBox* confidenceBox_ = nullptr;
    QLabel* providerNotice_ = nullptr;
    QLabel* modelStatusLabel_ = nullptr;
    QPushButton* analyzeButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;

    QProgressBar* progressBar_ = nullptr;
    QLabel* progressHeadline_ = nullptr;
    QLabel* progressCounters_ = nullptr;
    QLabel* progressProvenance_ = nullptr;

    QTreeWidget* runTree_ = nullptr;
    QLabel* runSummary_ = nullptr;
};

}  // namespace trace::ui
