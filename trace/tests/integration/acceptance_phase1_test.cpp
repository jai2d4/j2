// TRACE Phase 1 acceptance test (§41–§43).
//
// Drives the real application — ApplicationContext, MainWindow, the analysis
// panel, the detections table, the video overlay and the timeline lanes —
// through object detection end to end, then restarts it and checks that the
// results and the analyst's review survived.
//
// Three workflows are covered:
//   §41 a successful analysis and everything it must produce in the workspace
//   §42 a failed analysis: recorded as failed, never as complete
//   §43 a cancelled analysis: partial results kept, run recorded as cancelled
//
// The deterministic mock provider drives the UI workflows so they need no model
// and no GPU. A fourth test repeats the workflow with the real ONNX Runtime
// provider and skips itself, loudly, when the model or the clip is absent.
#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QThread>
#include <QTreeWidget>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "core/security/file_hasher.h"
#include "tests/support/test_environment.h"
#include "ui/analysis/analysis_panel.h"
#include "ui/analysis/detection_inspector.h"
#include "ui/analysis/detections_panel.h"
#include "ui/app/application_context.h"
#include "ui/app/main_window.h"
#include "ui/common/theme.h"
#include "ui/evidence_browser/evidence_panel.h"
#include "ui/timeline/timeline_widget.h"
#include "ui/viewer/video_view.h"
#include "ui/viewer/viewer_panel.h"

namespace trace {
namespace {

/// Signs in the way the application does, so the acceptance tests exercise the
/// real startup path rather than a permissive one that exists only for tests.
/// Since local accounts arrived, UserContext grants no authority until a
/// credential has been verified — a test that skipped this would be testing a
/// TRACE nobody runs.
bool signInForTest(trace::ui::ApplicationContext& context) {
    constexpr const char* kUser = "acceptance";
    constexpr const char* kPassword = "acceptance test password";
    auto needed = context.auth().needsFirstRunSetup();
    if (!needed) return false;
    if (needed.value()) {
        if (!context.auth().createFirstAdministrator(kUser, "Acceptance Operator", kPassword)) {
            return false;
        }
    }
    return context.auth().signIn(kUser, kPassword).ok();
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) return true;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

/// Dismisses any modal message box the application raises and remembers its
/// text. A failed analysis is reported to the operator through one of these,
/// and a modal dialog with nobody to answer it would block the test's own event
/// loop — the same way it would block a real operator's window.
class MessageBoxCollector : public QObject {
public:
    MessageBoxCollector() {
        timer_ = new QTimer(this);
        timer_->setInterval(25);
        connect(timer_, &QTimer::timeout, this, [this] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (box == nullptr) return;
            lastText_ = box->text();
            ++count_;
            box->accept();
        });
        timer_->start();
    }

    QString lastText() const { return lastText_; }
    int count() const { return count_; }

private:
    QTimer* timer_ = nullptr;
    QString lastText_;
    int count_ = 0;
};

/// A running TRACE with one case and one imported video, built through the
/// services so each test spends its time on the Phase 1 behaviour rather than
/// re-proving Phase 0 ingestion, which has its own acceptance test.
struct Workspace {
    std::unique_ptr<testing::TemporaryDirectory> dataRoot;
    std::unique_ptr<ui::ApplicationContext> context;
    std::unique_ptr<ui::MainWindow> window;
    std::unique_ptr<MessageBoxCollector> messageBoxes;
    Evidence evidence;
    std::string sourceDigest;

    static std::unique_ptr<Workspace> open(const std::filesystem::path& media,
                                           const std::string& prefix) {
        auto workspace = std::make_unique<Workspace>();
        workspace->messageBoxes = std::make_unique<MessageBoxCollector>();
        workspace->dataRoot = std::make_unique<testing::TemporaryDirectory>(prefix);

        const auto incoming = workspace->dataRoot->path() / "incoming";
        std::filesystem::create_directories(incoming);
        const auto source = incoming / media.filename();
        std::filesystem::copy_file(media, source);

        auto digest = hashFile(source);
        if (!digest) return nullptr;
        workspace->sourceDigest = digest.take();

        workspace->context = std::make_unique<ui::ApplicationContext>();
        if (!workspace->context->initialise(workspace->dataRoot->path())) return nullptr;
        if (!signInForTest(*workspace->context)) return nullptr;

        CaseDraft draft;
        draft.caseNumber = "CASE-0001";
        draft.title = "Detection acceptance";
        draft.investigator = "A. Analyst";
        auto created = workspace->context->cases().createCase(draft);
        if (!created) return nullptr;
        if (!workspace->context->openCase(QString::fromStdString(created.value().id))) {
            return nullptr;
        }

        IngestRequest request;
        request.caseId = created.value().id;
        request.sourcePath = source;
        auto ingested = workspace->context->evidence().ingest(request);
        if (!ingested) return nullptr;
        workspace->evidence = ingested.value().evidence;

        workspace->window = std::make_unique<ui::MainWindow>(workspace->context.get());
        workspace->window->resize(1600, 950);
        workspace->window->show();
        if (!waitFor([&] { return workspace->window->isVisible(); }, 10000)) return nullptr;

        workspace->window->evidencePanel()->refresh();
        workspace->window->evidencePanel()->selectEvidence(
            QString::fromStdString(workspace->evidence.id));
        if (!waitFor([&] { return workspace->context->currentEvidence().has_value(); }, 10000)) {
            return nullptr;
        }
        if (!waitFor([&] { return workspace->window->viewerPanel()->duration() > 0; }, 20000)) {
            return nullptr;
        }
        return workspace;
    }

    ~Workspace() {
        if (window != nullptr) {
            window->close();
            QApplication::processEvents();
        }
    }
};

/// Points the analysis panel at one provider/model pair through its real
/// combo boxes, as an operator would.
bool selectProvider(ui::AnalysisPanel* panel, const QString& providerId, const QString& modelId) {
    const int providerIndex = panel->providerBox()->findData(providerId);
    if (providerIndex < 0) return false;
    panel->providerBox()->setCurrentIndex(providerIndex);
    QApplication::processEvents();
    if (modelId.isEmpty()) return true;
    const int modelIndex = panel->modelBox()->findData(modelId);
    if (modelIndex < 0) return false;
    panel->modelBox()->setCurrentIndex(modelIndex);
    QApplication::processEvents();
    return true;
}

bool selectQuality(ui::AnalysisPanel* panel, AnalysisQuality quality) {
    const int index = panel->qualityBox()->findData(QString::fromUtf8(toString(quality)));
    if (index < 0) return false;
    panel->qualityBox()->setCurrentIndex(index);
    return true;
}

std::optional<AnalysisRun> runFor(ui::ApplicationContext& context, const std::string& evidenceId) {
    auto runs = context.analysis().runsForEvidence(evidenceId);
    if (!runs || runs.value().empty()) return std::nullopt;
    return runs.value().front();
}

// ═══════════════════════════════════════════════════ §41 successful analysis

TEST(AcceptancePhase1, ARunCanBeReviewedFromTheKeyboardAndInBulk) {
    // Reviewing ten thousand boxes with a mouse does not happen. This drives the
    // panel the way an analyst working a real run does — move, decide, move —
    // and then the sweep, checking that the progress indicator and the record
    // both keep up.
    auto workspace = Workspace::open(testing::sampleVideoPath(), "trace-phase1-review");
    ASSERT_NE(workspace, nullptr);

    auto* analysis = workspace->window->analysisPanel();
    auto* detections = workspace->window->detectionsPanel();

    ASSERT_TRUE(selectProvider(analysis, QStringLiteral("mock"), QString()));
    ASSERT_TRUE(selectQuality(analysis, AnalysisQuality::Standard));
    analysis->confidenceBox()->setValue(0.10);
    analysis->analyzeButton()->click();
    ASSERT_TRUE(waitFor([&] { return !analysis->analysisRunning(); }, 120000));
    QApplication::processEvents();
    ASSERT_TRUE(waitFor([&] { return detections->tree()->topLevelItemCount() > 2; }, 20000));

    // Nothing is reviewed yet, and the indicator says so rather than starting
    // somewhere plausible.
    ReviewProgress progress = detections->progress();
    const std::int64_t total = progress.total;
    ASSERT_GT(total, 2);
    EXPECT_EQ(progress.reviewed(), 0);
    EXPECT_DOUBLE_EQ(progress.fraction(), 0.0);

    // Move to the first row and rule on it. With auto-advance on, the selection
    // should land on something else afterwards — that is the whole point of the
    // keyboard path.
    detections->setAutoAdvance(true);
    detections->selectNext(false);
    const QString first = detections->selectedDetectionId();
    ASSERT_FALSE(first.isEmpty());

    detections->confirmSelected();
    QApplication::processEvents();
    EXPECT_NE(detections->selectedDetectionId(), first) << "auto-advance did not move on";

    progress = detections->progress();
    EXPECT_EQ(progress.reviewed(), 1);
    EXPECT_EQ(progress.confirmed, 1);
    EXPECT_EQ(progress.reviewedIndividually, 1);

    // A second decision, then walking back with the previous-item action.
    const QString second = detections->selectedDetectionId();
    ASSERT_FALSE(second.isEmpty());
    detections->markSelectedUncertain();
    QApplication::processEvents();
    EXPECT_EQ(detections->progress().reviewed(), 2);

    detections->selectPrevious();
    QApplication::processEvents();
    EXPECT_FALSE(detections->selectedDetectionId().isEmpty());

    // Now the sweep. skipConfirmation stands in for the operator pressing Yes;
    // the dialog itself is not what this is testing.
    auto swept = detections->reviewAllMatching(DetectionVerification::Rejected, true);
    ASSERT_TRUE(swept.has_value()) << "the bulk review did not run";
    EXPECT_EQ(*swept, total);
    QApplication::processEvents();

    progress = detections->progress();
    EXPECT_EQ(progress.reviewed(), total);
    EXPECT_EQ(progress.rejected, total);
    EXPECT_DOUBLE_EQ(progress.fraction(), 1.0);
    // The two individual reviews were swept over, so nothing remains that a
    // person ruled on one at a time. Claiming otherwise would overstate what was
    // examined.
    EXPECT_EQ(progress.reviewedIndividually, 0);

    // Rejected in bulk still means kept.
    DetectionQuery kept;
    kept.evidenceId = workspace->evidence.id;
    kept.includeRejected = true;
    auto remaining = workspace->context->analysis().countDetections(kept);
    ASSERT_TRUE(remaining.ok());
    EXPECT_EQ(remaining.take(), total) << "a bulk rejection deleted detections";

    // And each row says how it was reviewed, so a report cannot present the
    // sweep as individual examination.
    auto rows = workspace->context->analysis().detections(kept);
    ASSERT_TRUE(rows.ok());
    for (const auto& detection : rows.take()) {
        EXPECT_EQ(detection.reviewMethod, DetectionReviewMethod::Bulk);
    }
}

TEST(AcceptancePhase1, AnalyzeVideoProducesReviewableResultsThatSurviveARestart) {
    auto workspace = Workspace::open(testing::sampleVideoPath(), "trace-phase1-accept");
    ASSERT_NE(workspace, nullptr);
    std::cerr << "TRACE data root: " << workspace->dataRoot->path() << std::endl;

    const std::string evidenceId = workspace->evidence.id;
    auto* analysis = workspace->window->analysisPanel();
    auto* detections = workspace->window->detectionsPanel();
    auto* viewer = workspace->window->viewerPanel();
    auto* timeline = workspace->window->timeline();

    // 1. Nothing pretends results exist before a run.
    EXPECT_FALSE(analysis->resultsRun().has_value());
    EXPECT_FALSE(viewer->detectionOverlayAvailable());
    EXPECT_EQ(detections->tree()->topLevelItemCount(), 0);

    // 2. Configure and start the analysis through the real controls.
    ASSERT_TRUE(selectProvider(analysis, QStringLiteral("mock"), QString()));
    ASSERT_TRUE(selectQuality(analysis, AnalysisQuality::Standard));
    analysis->confidenceBox()->setValue(0.25);
    ASSERT_TRUE(analysis->analyzeButton()->isEnabled());

    analysis->analyzeButton()->click();
    ASSERT_TRUE(waitFor([&] { return !analysis->analysisRunning(); }, 120000))
        << "the analysis never finished";
    QApplication::processEvents();

    // 3. The run is recorded as completed, with full provenance.
    auto run = runFor(*workspace->context, evidenceId);
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run->status, AnalysisRunStatus::Completed);
    EXPECT_GT(run->framesAnalyzed, 0);
    EXPECT_GT(run->detectionsStored, 0);
    EXPECT_EQ(run->evidenceSha256, workspace->sourceDigest)
        << "the run must record the digest the evidence carried when it started";
    EXPECT_FALSE(run->deviceUsed.empty());
    EXPECT_TRUE(run->samplingIntervalUs.has_value());
    EXPECT_FALSE(run->errorMessage.has_value());

    // 4. TRACE never modifies the original: same bytes, same digest.
    const auto managed = workspace->context->layout().resolve(workspace->evidence.storageRelPath);
    auto afterAnalysis = hashFile(managed);
    ASSERT_TRUE(afterAnalysis.ok());
    EXPECT_EQ(afterAnalysis.value(), workspace->sourceDigest)
        << "analysis must not alter the managed original";

    // 5. The run appears in the panel's history and is the one being viewed.
    EXPECT_GE(analysis->runTree()->topLevelItemCount(), 1);
    ASSERT_TRUE(analysis->resultsRun().has_value());
    EXPECT_EQ(analysis->resultsRun()->id, run->id);

    // 6. The detections table lists them.
    ASSERT_TRUE(waitFor([&] { return detections->tree()->topLevelItemCount() > 0; }, 15000));
    const int listedRows = detections->tree()->topLevelItemCount();
    EXPECT_GT(listedRows, 0);
    EXPECT_GT(detections->totalMatching(), 0);

    // 7. Timeline lanes exist for the groups that were found, alongside the
    //    Phase 0 bookmark and note tracks.
    bool peopleLane = false;
    bool vehicleLane = false;
    for (const auto& track : timeline->tracks()) {
        if (track.name == QStringLiteral("People")) peopleLane = !track.markers.empty();
        if (track.name == QStringLiteral("Vehicles")) vehicleLane = !track.markers.empty();
    }
    EXPECT_TRUE(peopleLane) << "a People lane should appear once people have been detected";
    EXPECT_TRUE(vehicleLane) << "a Vehicles lane should appear once vehicles have been detected";

    // 8. Click-to-jump: activating a row moves the playhead to that moment.
    auto* firstRow = detections->tree()->topLevelItem(0);
    ASSERT_NE(firstRow, nullptr);
    const qint64 rowTimestamp = firstRow->data(0, Qt::UserRole + 1).toLongLong();
    detections->tree()->setCurrentItem(firstRow);
    QApplication::processEvents();
    emit detections->tree()->itemActivated(firstRow, 0);
    ASSERT_TRUE(waitFor([&] {
        return std::llabs(viewer->position() - rowTimestamp) < 100'000;
    }, 15000)) << "the viewer did not jump to the detection, position "
               << viewer->position() << " vs " << rowTimestamp;

    // 9. The overlay draws that frame's boxes over the video, and only that
    //    frame's — the overlay is display state, not a change to the image.
    EXPECT_TRUE(viewer->detectionOverlayAvailable());
    ASSERT_TRUE(waitFor([&] { return !viewer->videoView()->detections().empty(); }, 15000));
    const auto& overlay = viewer->videoView()->detections();
    EXPECT_FALSE(overlay.empty());
    for (const auto& item : overlay) {
        EXPECT_TRUE(item.box.valid()) << "overlay boxes must lie inside the frame";
        EXPECT_GT(item.confidence, 0.0);
        EXPECT_FALSE(item.label.isEmpty());
    }

    // 10. The detection inspector shows the provenance of the selected item.
    const QString selectedId = detections->selectedDetectionId();
    ASSERT_FALSE(selectedId.isEmpty());
    auto* inspector = workspace->window->detectionInspector();
    ASSERT_TRUE(waitFor([&] { return inspector->detection().has_value(); }, 10000));
    EXPECT_EQ(QString::fromStdString(inspector->detection()->id), selectedId);
    EXPECT_EQ(inspector->detection()->analysisRunId, run->id);

    // 11. Filtering narrows what is listed and drawn without deleting anything.
    const int allRows = detections->tree()->topLevelItemCount();
    detections->groupFilter()->setCurrentIndex(
        detections->groupFilter()->findData(QString::fromUtf8(toString(DetectionClassGroup::Person))));
    QApplication::processEvents();
    const int peopleRows = detections->tree()->topLevelItemCount();
    EXPECT_GT(peopleRows, 0);
    EXPECT_LE(peopleRows, allRows);
    for (int i = 0; i < peopleRows; ++i) {
        EXPECT_EQ(detections->tree()->topLevelItem(i)->text(2), QStringLiteral("People"));
    }
    detections->groupFilter()->setCurrentIndex(0);
    QApplication::processEvents();
    EXPECT_EQ(detections->tree()->topLevelItemCount(), allRows)
        << "clearing the filter must bring every detection back — none was deleted";

    // 12. Human verification: confirm one, reject another. Both are stored, and
    //     the rejected one is kept rather than removed.
    ASSERT_GE(detections->tree()->topLevelItemCount(), 2);
    detections->tree()->setCurrentItem(detections->tree()->topLevelItem(0));
    QApplication::processEvents();
    const QString confirmedId = detections->selectedDetectionId();
    ASSERT_FALSE(confirmedId.isEmpty());
    detections->confirmSelected();
    QApplication::processEvents();

    detections->tree()->setCurrentItem(detections->tree()->topLevelItem(1));
    QApplication::processEvents();
    const QString rejectedId = detections->selectedDetectionId();
    ASSERT_FALSE(rejectedId.isEmpty());
    ASSERT_NE(rejectedId, confirmedId);
    detections->rejectSelected();
    QApplication::processEvents();

    {
        auto confirmed =
            workspace->context->analysis().findDetection(confirmedId.toStdString());
        ASSERT_TRUE(confirmed.ok());
        ASSERT_TRUE(confirmed.value().has_value());
        EXPECT_EQ(confirmed.value()->verification, DetectionVerification::Confirmed);
        EXPECT_TRUE(confirmed.value()->verifiedAt.has_value());

        auto rejected = workspace->context->analysis().findDetection(rejectedId.toStdString());
        ASSERT_TRUE(rejected.ok());
        ASSERT_TRUE(rejected.value().has_value())
            << "a rejected detection must still exist — rejection is not deletion";
        EXPECT_EQ(rejected.value()->verification, DetectionVerification::Rejected);
    }

    // 13. The decisions are in the audit trail.
    {
        AuditQuery query;
        query.evidenceId = evidenceId;
        query.limit = 500;
        auto events = workspace->context->audit().list(query);
        ASSERT_TRUE(events.ok());
        const auto has = [&](AuditAction action) {
            return std::any_of(events.value().begin(), events.value().end(),
                               [&](const AuditEvent& event) { return event.action == action; });
        };
        EXPECT_TRUE(has(AuditAction::AnalysisStarted));
        EXPECT_TRUE(has(AuditAction::AnalysisCompleted));
        EXPECT_TRUE(has(AuditAction::DetectionConfirmed));
        EXPECT_TRUE(has(AuditAction::DetectionRejected));
    }

    const std::int64_t storedDetections = run->detectionsStored;
    const std::string runId = run->id;
    workspace->window->close();
    QApplication::processEvents();
    const auto dataRootPath = workspace->dataRoot->path();
    // Keep the directory alive past the first context's destruction.
    auto keepDirectory = std::move(workspace->dataRoot);
    workspace.reset();

    // ═════════════════════════════ RESTART ════════════════════════════════
    {
        ui::ApplicationContext context;
        ASSERT_TRUE(context.initialise(dataRootPath).ok());
        ASSERT_TRUE(signInForTest(context));
        ui::MainWindow window(&context);
        window.show();
        ASSERT_TRUE(waitFor([&] { return window.isVisible(); }, 10000));

        auto cases = context.cases().listCases();
        ASSERT_TRUE(cases.ok());
        ASSERT_EQ(cases.value().size(), 1u);
        ASSERT_TRUE(context.openCase(QString::fromStdString(cases.value().front().id)).ok());

        window.evidencePanel()->refresh();
        window.evidencePanel()->selectEvidence(QString::fromStdString(evidenceId));
        ASSERT_TRUE(waitFor([&] { return context.currentEvidence().has_value(); }, 15000));

        auto restored = runFor(context, evidenceId);
        ASSERT_TRUE(restored.has_value());
        EXPECT_EQ(restored->id, runId);
        EXPECT_EQ(restored->status, AnalysisRunStatus::Completed);
        EXPECT_EQ(restored->detectionsStored, storedDetections);

        ASSERT_TRUE(waitFor(
            [&] { return window.detectionsPanel()->tree()->topLevelItemCount() > 0; }, 15000))
            << "detections did not come back after a restart";

        auto confirmed = context.analysis().findDetection(confirmedId.toStdString());
        ASSERT_TRUE(confirmed.ok());
        ASSERT_TRUE(confirmed.value().has_value());
        EXPECT_EQ(confirmed.value()->verification, DetectionVerification::Confirmed)
            << "the analyst's review must survive a restart";

        auto rejected = context.analysis().findDetection(rejectedId.toStdString());
        ASSERT_TRUE(rejected.ok());
        ASSERT_TRUE(rejected.value().has_value());
        EXPECT_EQ(rejected.value()->verification, DetectionVerification::Rejected);

        // The evidence still verifies against its ingestion digest.
        auto verified = context.integrity().verify(*context.currentEvidence(),
                                                   context.currentCaseNumber().toStdString());
        ASSERT_TRUE(verified.ok());
        EXPECT_TRUE(verified.value().verified)
            << "the original must still match its recorded digest after an analysis";

        window.close();
        QApplication::processEvents();
    }
}

// ═══════════════════════════════════════════════════════ §42 failed analysis

TEST(AcceptancePhase1, AFailedAnalysisIsRecordedAsFailedAndNeverAsComplete) {
    auto workspace = Workspace::open(testing::sampleVideoPath(), "trace-phase1-fail");
    ASSERT_NE(workspace, nullptr);

    auto* analysis = workspace->window->analysisPanel();
    auto* detections = workspace->window->detectionsPanel();

    // yolox-s is in the catalogue but is not installed. TRACE must report that
    // rather than downloading it or quietly substituting another model.
    if (!selectProvider(analysis, QStringLiteral("onnxruntime"), QStringLiteral("yolox-s"))) {
        GTEST_SKIP() << "the ONNX Runtime provider is not compiled into this build";
    }
    auto missing = ModelManager::describe("yolox-s");
    ASSERT_TRUE(missing.has_value());
    ASSERT_FALSE(std::filesystem::exists(
        workspace->context->models().directory() / missing->fileName))
        << "this test needs a model that is genuinely absent";

    analysis->analyzeButton()->click();
    ASSERT_TRUE(waitFor([&] { return !analysis->analysisRunning(); }, 60000));
    QApplication::processEvents();

    auto run = runFor(*workspace->context, workspace->evidence.id);
    if (run.has_value()) {
        // A run that was created before the model was rejected must carry the
        // failure; it must never be presented as completed.
        EXPECT_EQ(run->status, AnalysisRunStatus::Failed);
        EXPECT_NE(run->status, AnalysisRunStatus::Completed);
        EXPECT_TRUE(run->errorMessage.has_value());
        EXPECT_EQ(run->detectionsStored, 0);
    }

    // The operator is told, in a dialog they have to acknowledge.
    EXPECT_GT(workspace->messageBoxes->count(), 0)
        << "a failed analysis must be reported, not swallowed";
    EXPECT_FALSE(workspace->messageBoxes->lastText().isEmpty());

    // Nothing was stored, and the workspace does not offer results.
    DetectionQuery query;
    query.evidenceId = workspace->evidence.id;
    auto counted = workspace->context->analysis().countDetections(query);
    ASSERT_TRUE(counted.ok());
    EXPECT_EQ(counted.value(), 0);
    EXPECT_EQ(detections->tree()->topLevelItemCount(), 0);
    EXPECT_FALSE(workspace->window->viewerPanel()->detectionOverlayAvailable());

    // The evidence is untouched.
    const auto managed = workspace->context->layout().resolve(workspace->evidence.storageRelPath);
    auto digest = hashFile(managed);
    ASSERT_TRUE(digest.ok());
    EXPECT_EQ(digest.value(), workspace->sourceDigest);

    // The failure is in the audit trail, and no completion event is.
    AuditQuery auditQuery;
    auditQuery.evidenceId = workspace->evidence.id;
    auditQuery.limit = 500;
    auto events = workspace->context->audit().list(auditQuery);
    ASSERT_TRUE(events.ok());
    const auto count = [&](AuditAction action) {
        return std::count_if(events.value().begin(), events.value().end(),
                             [&](const AuditEvent& event) { return event.action == action; });
    };
    EXPECT_GT(count(AuditAction::ModelValidationFailed) + count(AuditAction::AnalysisFailed), 0);
    EXPECT_EQ(count(AuditAction::AnalysisCompleted), 0);
}

// ════════════════════════════════════════════════════ §43 cancelled analysis

TEST(AcceptancePhase1, CancellingKeepsWhatWasFoundAndRecordsACancelledRun) {
    // Two ways to reach the same guarantee, because the mock provider finishes
    // a short clip faster than any interface could be asked to stop it:
    //
    //   real model available — run it over the long clip and press Cancel from
    //                          the panel's first progress update, part-way in
    //   otherwise            — press Cancel the instant the run starts, before
    //                          the decoder has opened the file
    //
    // Either way the run must end CANCELLED, never COMPLETED, and keep what it
    // had already stored.
    const auto clip = testing::pedestrianVideoPath();
    const bool realRun = testing::realDetectionModelAvailable() && clip.has_value();

    auto workspace = Workspace::open(clip.value_or(testing::sampleVideoPath()),
                                     "trace-phase1-cancel");
    ASSERT_NE(workspace, nullptr);

    auto* analysis = workspace->window->analysisPanel();
    ASSERT_TRUE(selectProvider(analysis,
                               realRun ? QStringLiteral("onnxruntime") : QStringLiteral("mock"),
                               realRun ? QStringLiteral("yolox-tiny") : QString()));
    // Every decoded frame: the longest run the interface offers.
    ASSERT_TRUE(selectQuality(analysis, AnalysisQuality::Detailed));

    bool cancelClicked = false;
    if (realRun) {
        QObject::connect(analysis, &ui::AnalysisPanel::progressUpdated, analysis,
                         [&](qint64 frames, qint64, double) {
                             if (cancelClicked || frames <= 0) return;
                             cancelClicked = true;
                             EXPECT_TRUE(analysis->cancelButton()->isEnabled());
                             analysis->cancelButton()->click();
                         });
    }

    analysis->analyzeButton()->click();
    ASSERT_TRUE(analysis->analysisRunning());
    if (!realRun) {
        ASSERT_TRUE(analysis->cancelButton()->isEnabled());
        analysis->cancelButton()->click();
        cancelClicked = true;
    }

    ASSERT_TRUE(waitFor([&] { return !analysis->analysisRunning(); }, 300000))
        << "cancellation did not stop the run";
    QApplication::processEvents();
    ASSERT_TRUE(cancelClicked) << "the run finished before Cancel could be pressed";

    auto run = runFor(*workspace->context, workspace->evidence.id);
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run->status, AnalysisRunStatus::Cancelled);
    EXPECT_NE(run->status, AnalysisRunStatus::Completed)
        << "a cancelled run must never be recorded as completed";
    EXPECT_FALSE(producedCompleteResults(run->status));
    EXPECT_TRUE(run->completedAt.has_value());

    // What was already found is kept — cancelling discards nothing.
    DetectionQuery query;
    query.evidenceId = workspace->evidence.id;
    auto counted = workspace->context->analysis().countDetections(query);
    ASSERT_TRUE(counted.ok());
    EXPECT_EQ(counted.value(), run->detectionsStored);
    if (realRun) {
        EXPECT_GT(run->framesAnalyzed, 0)
            << "the run should have analysed something before it was stopped";
    }

    // The evidence is untouched.
    const auto managed = workspace->context->layout().resolve(workspace->evidence.storageRelPath);
    auto digest = hashFile(managed);
    ASSERT_TRUE(digest.ok());
    EXPECT_EQ(digest.value(), workspace->sourceDigest);

    AuditQuery auditQuery;
    auditQuery.evidenceId = workspace->evidence.id;
    auditQuery.limit = 500;
    auto events = workspace->context->audit().list(auditQuery);
    ASSERT_TRUE(events.ok());
    const auto has = [&](AuditAction action) {
        return std::any_of(events.value().begin(), events.value().end(),
                           [&](const AuditEvent& event) { return event.action == action; });
    };
    EXPECT_TRUE(has(AuditAction::AnalysisCancelled));
    EXPECT_FALSE(has(AuditAction::AnalysisCompleted));

    // Cancelling is something the operator just chose to do; TRACE does not
    // report their own decision back to them as a failure.
    EXPECT_EQ(workspace->messageBoxes->count(), 0);
}

// ══════════════════════════════════════ the same workflow, with a real model

TEST(AcceptancePhase1, RealModelResultsAppearInTheWorkspace) {
    if (!testing::realDetectionModelAvailable()) {
        GTEST_SKIP() << "the YOLOX-Tiny artefact is not installed — run scripts/fetch_models.sh";
    }
    const auto clip = testing::pedestrianVideoPath();
    if (!clip) {
        GTEST_SKIP() << "the pedestrian clip is not present — run scripts/fetch_test_media.sh";
    }

    auto workspace = Workspace::open(*clip, "trace-phase1-real");
    ASSERT_NE(workspace, nullptr);

    auto* analysis = workspace->window->analysisPanel();
    auto* detections = workspace->window->detectionsPanel();
    auto* viewer = workspace->window->viewerPanel();

    ASSERT_TRUE(selectProvider(analysis, QStringLiteral("onnxruntime"),
                               QStringLiteral("yolox-tiny")));
    ASSERT_TRUE(selectQuality(analysis, AnalysisQuality::Fast));
    analysis->confidenceBox()->setValue(0.50);

    analysis->analyzeButton()->click();
    ASSERT_TRUE(waitFor([&] { return !analysis->analysisRunning(); }, 600000))
        << "the real analysis never finished";
    QApplication::processEvents();

    auto run = runFor(*workspace->context, workspace->evidence.id);
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run->status, AnalysisRunStatus::Completed);
    EXPECT_GT(run->detectionsStored, 0);
    ASSERT_TRUE(run->modelSha256.has_value());

    // The digest recorded on the run is the digest of the file that ran.
    auto descriptor = ModelManager::describe("yolox-tiny");
    ASSERT_TRUE(descriptor.has_value());
    auto onDisk = hashFile(workspace->context->models().pathFor(*descriptor));
    ASSERT_TRUE(onDisk.ok());
    EXPECT_EQ(*run->modelSha256, onDisk.value());

    ASSERT_TRUE(waitFor([&] { return detections->tree()->topLevelItemCount() > 0; }, 20000));

    // Real footage of a street: people must be among what was found, and every
    // box must lie inside the frame.
    DetectionQuery peopleQuery;
    peopleQuery.evidenceId = workspace->evidence.id;
    peopleQuery.classGroup = DetectionClassGroup::Person;
    auto people = workspace->context->analysis().countDetections(peopleQuery);
    ASSERT_TRUE(people.ok());
    EXPECT_GT(people.value(), 0) << "no people were detected in pedestrian footage";

    DetectionQuery allQuery;
    allQuery.evidenceId = workspace->evidence.id;
    auto all = workspace->context->analysis().detections(allQuery);
    ASSERT_TRUE(all.ok());
    for (const auto& detection : all.value()) {
        EXPECT_TRUE(detection.box.valid())
            << detection.classLabel << " box lies outside the frame";
        EXPECT_GE(detection.confidence, 0.50);
        EXPECT_LE(detection.confidence, 1.0);
    }

    // Jump to a detection and confirm the overlay draws it.
    auto* firstRow = detections->tree()->topLevelItem(0);
    ASSERT_NE(firstRow, nullptr);
    detections->tree()->setCurrentItem(firstRow);
    emit detections->tree()->itemActivated(firstRow, 0);
    ASSERT_TRUE(waitFor([&] { return !viewer->videoView()->detections().empty(); }, 20000));
}

}  // namespace
}  // namespace trace

int main(int argc, char** argv) {
    // Point the application at the development model store. TRACE_MODEL_DIR is
    // the same override an operator uses for a shared model directory, so the
    // test exercises the real lookup path rather than a test-only one.
    if (std::getenv("TRACE_MODEL_DIR") == nullptr) {
        trace::testing::setEnvironmentIfUnset("TRACE_MODEL_DIR",
                                              trace::testing::modelsDirectory().string());
    }
    QApplication application(argc, argv);
    trace::ui::applyTraceTheme(application);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
