// Phase 1 integration tests: the whole detection path against a real database
// and real media — run creation, persistence, provenance, cancellation, failure
// handling and survival across a restart.
//
// The deterministic mock provider carries the behavioural assertions so they
// hold on any machine. A separate test drives the real ONNX Runtime provider
// and skips itself, loudly, when the model artefact has not been fetched.
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>

#include "ai/detection/detection_provider_registry.h"
#include "ai/detection/models/model_manager.h"
#include "ai/detection/providers/mock_detection_provider.h"
#include "analysis/analysis_pipeline.h"
#include "core/security/file_hasher.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

class DetectionPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        registerBuiltinDetectionProviders();
        directory = std::make_unique<testing::TemporaryDirectory>("trace-detect");
        stack = std::make_unique<testing::TestStack>(testing::TestStack::create(directory->path()));

        source = directory->path() / "incoming" / "sample.mp4";
        std::filesystem::create_directories(source.parent_path());
        std::filesystem::copy_file(testing::sampleVideoPath(), source);

        CaseDraft draft;
        draft.title = "Detection";
        auto created = stack->cases->createCase(draft);
        ASSERT_TRUE(created.ok()) << created.error().toString();
        owner = created.take();

        IngestRequest request;
        request.caseId = owner.id;
        request.sourcePath = source;
        auto imported = stack->evidence->ingest(request);
        ASSERT_TRUE(imported.ok()) << imported.error().toString();
        evidence = imported.value().evidence;
    }

    DetectionAnalysisRequest baseRequest() const {
        DetectionAnalysisRequest request;
        request.caseId = owner.id;
        request.caseNumber = owner.caseNumber;
        request.evidenceId = evidence.id;
        request.evidenceNumber = evidence.evidenceNumber;
        request.mediaPath = stack->layout->resolve(evidence.storageRelPath);
        request.evidenceSha256 = evidence.sha256;
        request.providerId = MockDetectionProvider::kProviderId;
        request.quality = AnalysisQuality::Fast;
        request.confidenceThreshold = 0.50;
        return request;
    }

    AnalysisPipeline makePipeline() const {
        return AnalysisPipeline(stack->analysis, ModelManager(testing::modelsDirectory()),
                                *stack->layout);
    }

    bool auditContains(AuditAction action) const {
        AuditQuery query;
        query.action = action;
        auto events = stack->audit->list(query);
        return events.ok() && !events.value().empty();
    }

    std::unique_ptr<testing::TemporaryDirectory> directory;
    std::unique_ptr<testing::TestStack> stack;
    std::filesystem::path source;
    Case owner;
    Evidence evidence;
};

TEST_F(DetectionPipelineTest, StoresDetectionsWithFullProvenanceAndCompletesTheRun) {
    auto pipeline = makePipeline();
    auto result = pipeline.execute(baseRequest());
    ASSERT_TRUE(result.ok()) << result.error().toString();
    const AnalysisOutcome outcome = result.take();

    EXPECT_EQ(outcome.status, AnalysisRunStatus::Completed);
    EXPECT_GT(outcome.framesAnalyzed, 0);
    EXPECT_GT(outcome.detectionsStored, 0);

    // The run is persisted with the state it actually reached.
    auto stored = stack->analysis->findRun(outcome.run.id);
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    const AnalysisRun& run = *stored.value();
    EXPECT_EQ(run.status, AnalysisRunStatus::Completed);
    EXPECT_TRUE(run.completedAt.has_value());
    EXPECT_FALSE(run.errorMessage.has_value());
    EXPECT_EQ(run.evidenceId, evidence.id);
    EXPECT_EQ(run.caseId, owner.id);
    EXPECT_EQ(run.evidenceSha256, evidence.sha256)
        << "the run must record the evidence digest it analysed";
    EXPECT_EQ(run.framesAnalyzed, outcome.framesAnalyzed);
    EXPECT_EQ(run.detectionsStored, outcome.detectionsStored);
    EXPECT_FALSE(run.configurationJson.empty());
    EXPECT_NE(run.configurationJson.find("confidence_threshold"), std::string::npos);

    // Every detection traces to case, evidence, run and a real timestamp.
    DetectionQuery query;
    query.evidenceId = evidence.id;
    auto detections = stack->analysis->detections(query);
    ASSERT_TRUE(detections.ok());
    ASSERT_FALSE(detections.value().empty());

    const auto& first = detections.value().front();
    EXPECT_EQ(first.analysisRunId, run.id);
    EXPECT_EQ(first.evidenceId, evidence.id);
    EXPECT_EQ(first.caseId, owner.id);
    EXPECT_EQ(first.verification, DetectionVerification::Unreviewed)
        << "machine output must not be self-certified";
    EXPECT_GE(first.timestampUs, 0);

    for (const auto& detection : detections.value()) {
        EXPECT_GT(detection.confidence, 0.0);
        EXPECT_LE(detection.confidence, 1.0);
        EXPECT_TRUE(detection.box.valid()) << "boxes must lie inside the frame";
        EXPECT_FALSE(detection.classLabel.empty());
        EXPECT_TRUE(detection.pixelWidth.has_value());
        EXPECT_GT(*detection.pixelWidth, 0);
    }

    // Timestamps are ordered and unique per analysed frame.
    std::vector<std::int64_t> timestamps;
    for (const auto& detection : detections.value()) timestamps.push_back(detection.timestampUs);
    EXPECT_TRUE(std::is_sorted(timestamps.begin(), timestamps.end()));

    EXPECT_TRUE(auditContains(AuditAction::AnalysisStarted));
    EXPECT_TRUE(auditContains(AuditAction::AnalysisCompleted));
    EXPECT_TRUE(auditContains(AuditAction::ModelLoaded));
}

TEST_F(DetectionPipelineTest, SamplingRespectsTheRequestedQuality) {
    auto pipeline = makePipeline();
    auto request = baseRequest();
    request.quality = AnalysisQuality::Fast;  // ~5 per second over an 8s clip
    auto fast = pipeline.execute(request);
    ASSERT_TRUE(fast.ok()) << fast.error().toString();

    request.quality = AnalysisQuality::Standard;  // ~10 per second
    auto standard = pipeline.execute(request);
    ASSERT_TRUE(standard.ok()) << standard.error().toString();

    EXPECT_GT(standard.value().framesAnalyzed, fast.value().framesAnalyzed)
        << "a denser quality setting must analyse more frames";

    // The rule is a minimum spacing in media time, not a fabricated rate: this
    // clip has a frame every 40 ms, so a 100 ms target is met by taking every
    // third frame (120 ms) and a 200 ms target by every fifth (200 ms). TRACE
    // samples what the recording actually contains rather than interpolating.
    const auto analysedTimestamps = [&](const std::string& runId) {
        DetectionQuery query;
        query.analysisRunId = runId;
        auto detections = stack->analysis->detections(query);
        EXPECT_TRUE(detections.ok());
        std::vector<std::int64_t> distinct;
        for (const auto& detection : detections.value()) {
            if (distinct.empty() || distinct.back() != detection.timestampUs) {
                distinct.push_back(detection.timestampUs);
            }
        }
        return distinct;
    };

    for (const auto& [runId, interval] :
         {std::pair<std::string, std::int64_t>{fast.value().run.id, 200'000},
          std::pair<std::string, std::int64_t>{standard.value().run.id, 100'000}}) {
        const auto timestamps = analysedTimestamps(runId);
        ASSERT_GT(timestamps.size(), 1u);
        for (std::size_t i = 1; i < timestamps.size(); ++i) {
            EXPECT_GE(timestamps[i] - timestamps[i - 1], interval)
                << "analysed frames must be at least one sampling interval apart";
        }
    }

    // 8 seconds sampled every 200 ms and every 120 ms respectively.
    EXPECT_NEAR(static_cast<double>(fast.value().framesAnalyzed), 40.0, 2.0);
    EXPECT_NEAR(static_cast<double>(standard.value().framesAnalyzed), 67.0, 2.0);
}

TEST_F(DetectionPipelineTest, ConfidenceThresholdRemovesWeakDetections) {
    auto pipeline = makePipeline();
    auto request = baseRequest();
    request.confidenceThreshold = 0.20;  // keeps the mock's low-confidence object
    auto permissive = pipeline.execute(request);
    ASSERT_TRUE(permissive.ok());

    request.confidenceThreshold = 0.80;  // keeps only the strongest
    auto strict = pipeline.execute(request);
    ASSERT_TRUE(strict.ok());

    EXPECT_GT(permissive.value().detectionsStored, strict.value().detectionsStored);

    DetectionQuery query;
    query.analysisRunId = strict.value().run.id;
    auto detections = stack->analysis->detections(query);
    ASSERT_TRUE(detections.ok());
    for (const auto& detection : detections.value()) {
        EXPECT_GE(detection.confidence, 0.80);
    }
}

TEST_F(DetectionPipelineTest, CancellationNeverReportsCompletion) {
    auto pipeline = makePipeline();
    auto request = baseRequest();
    request.quality = AnalysisQuality::Detailed;  // give it enough work to interrupt

    int updates = 0;
    auto result = pipeline.execute(request, [&](const AnalysisProgress&) {
        ++updates;
        return false;  // cancel at the first opportunity
    });
    ASSERT_TRUE(result.ok()) << result.error().toString();
    const AnalysisOutcome outcome = result.take();

    EXPECT_EQ(outcome.status, AnalysisRunStatus::Cancelled);
    EXPECT_NE(outcome.status, AnalysisRunStatus::Completed);

    auto stored = stack->analysis->findRun(outcome.run.id);
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    EXPECT_EQ(stored.value()->status, AnalysisRunStatus::Cancelled);
    EXPECT_FALSE(producedCompleteResults(stored.value()->status));
    EXPECT_TRUE(stored.value()->completedAt.has_value());
    EXPECT_TRUE(stored.value()->errorMessage.has_value());

    // Work done before the stop is kept, and the counters agree with it.
    DetectionQuery query;
    query.analysisRunId = outcome.run.id;
    auto count = stack->analysis->countDetections(query);
    ASSERT_TRUE(count.ok());
    EXPECT_EQ(count.value(), stored.value()->detectionsStored);

    EXPECT_TRUE(auditContains(AuditAction::AnalysisCancelled));

    // The evidence itself is untouched by an interrupted analysis.
    auto verified = stack->integrity->verify(evidence, owner.caseNumber);
    ASSERT_TRUE(verified.ok());
    EXPECT_TRUE(verified.value().verified);
}

TEST_F(DetectionPipelineTest, AnInvalidModelFailsTheRunWithoutTouchingEvidence) {
    // A file of the right name whose content is not the model: this is the
    // controlled invalid configuration of the failure acceptance test.
    const auto modelDirectory = directory->path() / "models";
    std::filesystem::create_directories(modelDirectory);
    const auto descriptor = ModelManager::describe("yolox-tiny");
    ASSERT_TRUE(descriptor.has_value());
    { std::ofstream(modelDirectory / descriptor->fileName, std::ios::binary) << "corrupt"; }

    AnalysisPipeline pipeline(stack->analysis, ModelManager(modelDirectory), *stack->layout);
    auto request = baseRequest();
    request.providerId = "onnxruntime";
    request.modelId = "yolox-tiny";

    auto result = pipeline.execute(request);
    ASSERT_TRUE(result.ok()) << "a rejected model is a recorded failure, not a crash";
    const AnalysisOutcome outcome = result.take();

    EXPECT_EQ(outcome.status, AnalysisRunStatus::Failed);
    ASSERT_TRUE(outcome.error.has_value());
    EXPECT_NE(outcome.error->find("does not match"), std::string::npos);

    auto stored = stack->analysis->findRun(outcome.run.id);
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    EXPECT_EQ(stored.value()->status, AnalysisRunStatus::Failed);
    EXPECT_TRUE(stored.value()->errorMessage.has_value());
    EXPECT_EQ(stored.value()->detectionsStored, 0);

    EXPECT_TRUE(auditContains(AuditAction::ModelValidationFailed));
    EXPECT_TRUE(auditContains(AuditAction::AnalysisFailed));

    // Phase 0 state is intact: evidence still verifies, and the case is usable.
    auto verified = stack->integrity->verify(evidence, owner.caseNumber);
    ASSERT_TRUE(verified.ok());
    EXPECT_TRUE(verified.value().verified);
    auto reopened = stack->cases->findByCaseNumber(owner.caseNumber);
    ASSERT_TRUE(reopened.ok());
    EXPECT_TRUE(reopened.value().has_value());
}

TEST_F(DetectionPipelineTest, AMissingModelIsReportedRatherThanDownloaded) {
    const auto emptyModels = directory->path() / "no-models";
    std::filesystem::create_directories(emptyModels);

    AnalysisPipeline pipeline(stack->analysis, ModelManager(emptyModels), *stack->layout);
    auto request = baseRequest();
    request.providerId = "onnxruntime";
    request.modelId = "yolox-tiny";

    auto result = pipeline.execute(request);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().status, AnalysisRunStatus::Failed);
    ASSERT_TRUE(result.value().error.has_value());
    EXPECT_NE(result.value().error->find("not found"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_empty(emptyModels)) << "nothing may be fetched silently";
}

TEST_F(DetectionPipelineTest, UnknownProviderIsRejectedBeforeAnyRunIsCreated) {
    auto pipeline = makePipeline();
    auto request = baseRequest();
    request.providerId = "not-installed";

    auto result = pipeline.execute(request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);

    auto runs = stack->analysis->runsForEvidence(evidence.id);
    ASSERT_TRUE(runs.ok());
    EXPECT_TRUE(runs.value().empty());
}

TEST_F(DetectionPipelineTest, ResultsAndVerificationSurviveARestart) {
    std::string runId;
    std::string detectionId;
    std::int64_t detectionCount = 0;

    {
        auto pipeline = makePipeline();
        auto result = pipeline.execute(baseRequest());
        ASSERT_TRUE(result.ok()) << result.error().toString();
        runId = result.value().run.id;
        detectionCount = result.value().detectionsStored;

        DetectionQuery query;
        query.evidenceId = evidence.id;
        auto detections = stack->analysis->detections(query);
        ASSERT_TRUE(detections.ok());
        ASSERT_FALSE(detections.value().empty());
        const Detection subject = detections.value().front();
        detectionId = subject.id;

        ASSERT_TRUE(stack->analysis
                        ->setVerification(subject, DetectionVerification::Confirmed,
                                          "Matches the person visible at this moment",
                                          owner.caseNumber, evidence.evidenceNumber)
                        .ok());
        // Closing the stack drops the database connection, as closing TRACE does.
        stack.reset();
    }

    auto reopened = testing::TestStack::create(directory->path());

    auto run = reopened.analysis->findRun(runId);
    ASSERT_TRUE(run.ok());
    ASSERT_TRUE(run.value().has_value()) << "the analysis run did not survive the restart";
    EXPECT_EQ(run.value()->status, AnalysisRunStatus::Completed);
    EXPECT_EQ(run.value()->detectionsStored, detectionCount);
    EXPECT_FALSE(run.value()->providerName.empty());
    EXPECT_FALSE(run.value()->modelName.empty());

    DetectionQuery query;
    query.evidenceId = evidence.id;
    auto detections = reopened.analysis->detections(query);
    ASSERT_TRUE(detections.ok());
    EXPECT_EQ(static_cast<std::int64_t>(detections.value().size()), detectionCount);

    auto subject = reopened.analysis->findDetection(detectionId);
    ASSERT_TRUE(subject.ok());
    ASSERT_TRUE(subject.value().has_value());
    EXPECT_EQ(subject.value()->verification, DetectionVerification::Confirmed)
        << "the analyst's decision did not survive the restart";
    EXPECT_TRUE(subject.value()->verifiedBy.has_value());
    EXPECT_TRUE(subject.value()->verifiedAt.has_value());
    EXPECT_FALSE(subject.value()->analystNote.empty());
}

TEST_F(DetectionPipelineTest, RejectedDetectionsAreKeptAndAudited) {
    auto pipeline = makePipeline();
    auto result = pipeline.execute(baseRequest());
    ASSERT_TRUE(result.ok());

    DetectionQuery query;
    query.evidenceId = evidence.id;
    auto detections = stack->analysis->detections(query);
    ASSERT_TRUE(detections.ok());
    ASSERT_FALSE(detections.value().empty());
    const Detection subject = detections.value().front();

    ASSERT_TRUE(stack->analysis
                    ->setVerification(subject, DetectionVerification::Rejected, "Shadow, not a person",
                                      owner.caseNumber, evidence.evidenceNumber)
                    .ok());

    // Rejection is a judgement, not a deletion: the row stays.
    auto stored = stack->analysis->findDetection(subject.id);
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    EXPECT_EQ(stored.value()->verification, DetectionVerification::Rejected);

    // ...but a view that hides rejections no longer returns it.
    DetectionQuery visible;
    visible.evidenceId = evidence.id;
    visible.includeRejected = false;
    auto remaining = stack->analysis->detections(visible);
    ASSERT_TRUE(remaining.ok());
    EXPECT_TRUE(std::none_of(remaining.value().begin(), remaining.value().end(),
                             [&](const Detection& d) { return d.id == subject.id; }));

    EXPECT_TRUE(auditContains(AuditAction::DetectionRejected));
}

TEST_F(DetectionPipelineTest, OverlayQueryReturnsOneAnalysedFrameAtATime) {
    auto pipeline = makePipeline();
    auto result = pipeline.execute(baseRequest());
    ASSERT_TRUE(result.ok());

    DetectionQuery query;
    query.evidenceId = evidence.id;
    auto all = stack->analysis->detections(query);
    ASSERT_TRUE(all.ok());
    ASSERT_FALSE(all.value().empty());
    const std::int64_t someTimestamp = all.value().front().timestampUs;

    auto near = stack->analysis->detectionsNearTimestamp(evidence.id, someTimestamp + 5'000,
                                                         200'000, 0.0, true);
    ASSERT_TRUE(near.ok());
    ASSERT_FALSE(near.value().empty());
    // Every returned detection belongs to the same analysed frame, so boxes from
    // two samples are never drawn together.
    for (const auto& detection : near.value()) {
        EXPECT_EQ(detection.timestampUs, near.value().front().timestampUs);
    }

    // Far from any analysed frame, nothing is returned rather than the nearest.
    auto empty = stack->analysis->detectionsNearTimestamp(evidence.id, 60'000'000, 50'000, 0.0, true);
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty.value().empty());
}

// ------------------------------------------------------- real model inference

TEST_F(DetectionPipelineTest, RealModelDetectsPeopleAndVehiclesInTestFootage) {
    if (!testing::realDetectionModelAvailable()) {
        GTEST_SKIP() << "YOLOX-Tiny is not installed in " << testing::modelsDirectory()
                     << " — run scripts/fetch_models.sh to enable real inference tests.";
    }
    const auto footage = testing::pedestrianVideoPath();
    if (!footage) {
        GTEST_SKIP() << "Test footage is not installed — run scripts/fetch_test_media.sh.";
    }

    // Import the pedestrian clip as a second evidence item, through the real
    // ingestion path, so the analysis runs against managed evidence.
    IngestRequest ingest;
    ingest.caseId = owner.id;
    ingest.sourcePath = *footage;
    auto imported = stack->evidence->ingest(ingest);
    ASSERT_TRUE(imported.ok()) << imported.error().toString();
    const Evidence footageEvidence = imported.value().evidence;

    AnalysisPipeline pipeline(stack->analysis, ModelManager(testing::modelsDirectory()),
                              *stack->layout);
    DetectionAnalysisRequest request;
    request.caseId = owner.id;
    request.caseNumber = owner.caseNumber;
    request.evidenceId = footageEvidence.id;
    request.evidenceNumber = footageEvidence.evidenceNumber;
    request.mediaPath = stack->layout->resolve(footageEvidence.storageRelPath);
    request.evidenceSha256 = footageEvidence.sha256;
    request.providerId = "onnxruntime";
    request.modelId = "yolox-tiny";
    request.quality = AnalysisQuality::Fast;
    request.confidenceThreshold = 0.50;
    request.device = DevicePreference::Auto;
    request.analyseUpToUs = 4'000'000;  // four seconds keeps the suite quick

    auto result = pipeline.execute(request);
    ASSERT_TRUE(result.ok()) << result.error().toString();
    const AnalysisOutcome outcome = result.take();

    EXPECT_EQ(outcome.status, AnalysisRunStatus::Completed) << outcome.error.value_or("");
    EXPECT_GT(outcome.framesAnalyzed, 0);
    EXPECT_GT(outcome.detectionsStored, 0);
    EXPECT_FALSE(outcome.deviceUsed.empty());

    // The run names the exact artefact that produced these results.
    auto run = stack->analysis->findRun(outcome.run.id);
    ASSERT_TRUE(run.ok());
    ASSERT_TRUE(run.value().has_value());
    ASSERT_TRUE(run.value()->modelSha256.has_value());
    EXPECT_EQ(run.value()->modelSha256->size(), 64u);
    EXPECT_FALSE(run.value()->modelName.empty());
    EXPECT_FALSE(run.value()->modelVersion.empty());
    EXPECT_NE(run.value()->runtime.find("ONNX Runtime"), std::string::npos);

    // The digest recorded is the digest of the file on disk.
    ModelManager manager(testing::modelsDirectory());
    const auto descriptor = ModelManager::describe("yolox-tiny");
    ASSERT_TRUE(descriptor.has_value());
    auto onDisk = hashFile(manager.pathFor(*descriptor));
    ASSERT_TRUE(onDisk.ok());
    EXPECT_EQ(*run.value()->modelSha256, onDisk.value());

    DetectionQuery query;
    query.evidenceId = footageEvidence.id;
    auto detections = stack->analysis->detections(query);
    ASSERT_TRUE(detections.ok());
    ASSERT_FALSE(detections.value().empty());

    int people = 0;
    int vehicles = 0;
    for (const auto& detection : detections.value()) {
        EXPECT_GE(detection.confidence, 0.50);
        EXPECT_LE(detection.confidence, 1.0);
        EXPECT_TRUE(detection.box.valid())
            << detection.classLabel << " box outside the frame: " << detection.box.x << ","
            << detection.box.y << " " << detection.box.width << "x" << detection.box.height;
        EXPECT_GE(detection.timestampUs, 0);
        EXPECT_LE(detection.timestampUs, request.analyseUpToUs + 1'000'000);
        if (detection.classGroup == DetectionClassGroup::Person) ++people;
        if (detection.classGroup == DetectionClassGroup::Vehicle) ++vehicles;
    }
    // This clip is pedestrians crossing in front of parked cars.
    EXPECT_GT(people, 0) << "no people were detected in footage that contains people";
    EXPECT_GT(vehicles, 0) << "no vehicles were detected in footage that contains vehicles";

    // The original is unchanged by having been analysed.
    auto verified = stack->integrity->verify(footageEvidence, owner.caseNumber);
    ASSERT_TRUE(verified.ok());
    EXPECT_TRUE(verified.value().verified);
}

}  // namespace
}  // namespace trace
