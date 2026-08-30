// Reviewing detections at scale.
//
// The capability is straightforward; what needs testing is the constraint that
// comes with it. A sweep and an examination reach the same verification state by
// very different means, and the case file has to be able to tell them apart —
// in the rows, in the audit trail, and in anything derived from either. A test
// suite that only checked "the rows changed" would pass on an implementation
// that quietly presented ten thousand sweeps as ten thousand reviews.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "ai/detection/detection_provider_registry.h"
#include "ai/detection/providers/mock_detection_provider.h"
#include "analysis/analysis_pipeline.h"
#include "core/models/detection.h"
#include "core/services/analysis_service.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

class BulkReviewTest : public ::testing::Test {
protected:
    void SetUp() override {
        registerBuiltinDetectionProviders();
        directory = std::make_unique<testing::TemporaryDirectory>("trace-bulk");
        stack = std::make_unique<testing::TestStack>(testing::TestStack::create(directory->path()));

        const auto source = directory->path() / "incoming" / "sample.mp4";
        std::filesystem::create_directories(source.parent_path());
        std::filesystem::copy_file(testing::sampleVideoPath(), source);

        CaseDraft draft;
        draft.title = "Bulk review";
        auto created = stack->cases->createCase(draft);
        ASSERT_TRUE(created.ok()) << created.error().toString();
        owner = created.take();

        IngestRequest request;
        request.caseId = owner.id;
        request.sourcePath = source;
        auto imported = stack->evidence->ingest(request);
        ASSERT_TRUE(imported.ok()) << imported.error().toString();
        evidence = imported.value().evidence;

        // A real run, so the detections under test are the ones the pipeline
        // actually produces rather than rows invented for the occasion.
        DetectionAnalysisRequest analysis;
        analysis.caseId = owner.id;
        analysis.caseNumber = owner.caseNumber;
        analysis.evidenceId = evidence.id;
        analysis.evidenceNumber = evidence.evidenceNumber;
        analysis.mediaPath = stack->layout->resolve(evidence.storageRelPath);
        analysis.evidenceSha256 = evidence.sha256;
        analysis.providerId = MockDetectionProvider::kProviderId;
        analysis.quality = AnalysisQuality::Fast;
        analysis.confidenceThreshold = 0.10;

        AnalysisPipeline pipeline(stack->analysis, ModelManager(testing::modelsDirectory()),
                                  *stack->layout);
        auto executed = pipeline.execute(analysis);
        ASSERT_TRUE(executed.ok()) << executed.error().toString();
        run = executed.take().run;

        auto detections = stack->analysis->detections(runQuery());
        ASSERT_TRUE(detections.ok());
        ASSERT_GE(detections.take().size(), 4u) << "not enough detections to sweep";
    }

    DetectionQuery runQuery() const {
        DetectionQuery query;
        query.evidenceId = evidence.id;
        query.analysisRunId = run.id;
        return query;
    }

    std::vector<Detection> allDetections() const {
        auto detections = stack->analysis->detections(runQuery());
        EXPECT_TRUE(detections.ok());
        return detections.ok() ? detections.take() : std::vector<Detection>{};
    }

    std::int64_t auditCount(AuditAction action) const {
        AuditQuery query;
        query.action = action;
        auto events = stack->audit->list(query);
        return events.ok() ? static_cast<std::int64_t>(events.take().size()) : -1;
    }

    std::unique_ptr<testing::TemporaryDirectory> directory;
    std::unique_ptr<testing::TestStack> stack;
    Case owner;
    Evidence evidence;
    AnalysisRun run;
};

TEST_F(BulkReviewTest, OneSweepRulesOnEverythingMatchingAndReportsHowMany) {
    const auto before = allDetections();
    ASSERT_FALSE(before.empty());
    for (const Detection& detection : before) {
        ASSERT_EQ(detection.verification, DetectionVerification::Unreviewed);
        ASSERT_EQ(detection.reviewMethod, DetectionReviewMethod::NotReviewed);
    }

    auto swept = stack->analysis->setVerificationForQuery(
        runQuery(), DetectionVerification::Confirmed, "swept after review of the sequence",
        owner.caseNumber, evidence.evidenceNumber, "the whole run, unfiltered");
    ASSERT_TRUE(swept.ok()) << swept.error().toString();
    EXPECT_EQ(swept.take(), static_cast<std::int64_t>(before.size()));

    for (const Detection& detection : allDetections()) {
        EXPECT_EQ(detection.verification, DetectionVerification::Confirmed);
        EXPECT_EQ(detection.reviewMethod, DetectionReviewMethod::Bulk);
        EXPECT_TRUE(detection.verifiedBy.has_value());
        EXPECT_TRUE(detection.verifiedAt.has_value());
    }
}

TEST_F(BulkReviewTest, ASweepIsOneAuditRecordThatSaysItWasASweep) {
    // The point of the whole design. Writing one record per detection would make
    // a sweep indistinguishable from diligent individual review to anyone
    // auditing the case afterwards.
    const std::int64_t before = auditCount(AuditAction::DetectionConfirmed);
    const auto detections = allDetections();
    ASSERT_GT(detections.size(), 1u);

    ASSERT_TRUE(stack->analysis
                    ->setVerificationForQuery(runQuery(), DetectionVerification::Confirmed, "",
                                              owner.caseNumber, evidence.evidenceNumber,
                                              "class group Person, confidence at or above 0.50")
                    .ok());

    const std::int64_t after = auditCount(AuditAction::DetectionConfirmed);
    EXPECT_EQ(after, before + 1) << "a sweep wrote one record per detection";

    AuditQuery query;
    query.action = AuditAction::DetectionConfirmed;
    auto events = stack->audit->list(query);
    ASSERT_TRUE(events.ok());
    const auto rows = events.take();
    ASSERT_FALSE(rows.empty());

    const auto& latest = rows.front();
    // Legible in the description itself, not only in the JSON: somebody reading
    // the audit trail should not have to open the details to see it was a sweep.
    EXPECT_NE(latest.description.find("Bulk review"), std::string::npos) << latest.description;
    EXPECT_NE(latest.description.find(std::to_string(detections.size())), std::string::npos)
        << latest.description;
    const std::string details = latest.detailsJson;
    EXPECT_NE(details.find("\"bulk\":true"), std::string::npos) << details;
    EXPECT_NE(details.find("class group Person"), std::string::npos) << details;
    EXPECT_NE(details.find("\"detections_retained\":true"), std::string::npos) << details;
}

TEST_F(BulkReviewTest, ASweepRespectsTheFilterItWasGiven) {
    const auto detections = allDetections();
    ASSERT_GE(detections.size(), 2u);

    // Sweep only the first half of the recording.
    std::int64_t latest = 0;
    for (const Detection& detection : detections) latest = std::max(latest, detection.timestampUs);
    const std::int64_t cutoff = latest / 2;

    DetectionQuery query = runQuery();
    query.toUs = cutoff;
    auto expected = stack->analysis->countDetections(query);
    ASSERT_TRUE(expected.ok());
    const std::int64_t matching = expected.take();
    ASSERT_GT(matching, 0);
    ASSERT_LT(matching, static_cast<std::int64_t>(detections.size()))
        << "the cutoff selected everything, so this proves nothing";

    auto swept = stack->analysis->setVerificationForQuery(
        query, DetectionVerification::Rejected, "", owner.caseNumber, evidence.evidenceNumber,
        "up to the halfway point");
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.take(), matching);

    for (const Detection& detection : allDetections()) {
        if (detection.timestampUs <= cutoff) {
            EXPECT_EQ(detection.verification, DetectionVerification::Rejected);
        } else {
            EXPECT_EQ(detection.verification, DetectionVerification::Unreviewed)
                << "a detection outside the filter was swept";
        }
    }
}

TEST_F(BulkReviewTest, RejectingInBulkKeepsEveryDetection) {
    // The Phase 1 rule, which a bulk path could quietly break: rejection marks,
    // it does not delete.
    const std::size_t before = allDetections().size();
    ASSERT_TRUE(stack->analysis
                    ->setVerificationForQuery(runQuery(), DetectionVerification::Rejected, "",
                                              owner.caseNumber, evidence.evidenceNumber, "all")
                    .ok());

    DetectionQuery query = runQuery();
    query.includeRejected = true;
    auto after = stack->analysis->detections(query);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.take().size(), before) << "bulk rejection deleted detections";
}

TEST_F(BulkReviewTest, ASweepOverwritesAnIndividualReviewAndSaysSo) {
    // Somebody examined one box; a later sweep covered it. As of the sweep, the
    // state is the sweep's doing, and claiming otherwise would credit the
    // current value to an examination that did not produce it.
    auto detections = allDetections();
    ASSERT_FALSE(detections.empty());
    const Detection first = detections.front();

    ASSERT_TRUE(stack->analysis
                    ->setVerification(first, DetectionVerification::Uncertain, "looked at this one",
                                      owner.caseNumber, evidence.evidenceNumber)
                    .ok());
    {
        auto reloaded = stack->analysis->findDetection(first.id);
        ASSERT_TRUE(reloaded.ok());
        const auto value = reloaded.take();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value->reviewMethod, DetectionReviewMethod::Individual);
    }

    ASSERT_TRUE(stack->analysis
                    ->setVerificationForQuery(runQuery(), DetectionVerification::Confirmed, "",
                                              owner.caseNumber, evidence.evidenceNumber, "all")
                    .ok());

    auto reloaded = stack->analysis->findDetection(first.id);
    ASSERT_TRUE(reloaded.ok());
    const auto value = reloaded.take();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->verification, DetectionVerification::Confirmed);
    EXPECT_EQ(value->reviewMethod, DetectionReviewMethod::Bulk);
}

TEST_F(BulkReviewTest, ClearingAReviewClearsHowItWasMade) {
    auto detections = allDetections();
    ASSERT_FALSE(detections.empty());
    const Detection first = detections.front();

    ASSERT_TRUE(stack->analysis
                    ->setVerification(first, DetectionVerification::Confirmed, "", owner.caseNumber,
                                      evidence.evidenceNumber)
                    .ok());
    ASSERT_TRUE(stack->analysis
                    ->setVerification(first, DetectionVerification::Unreviewed, "",
                                      owner.caseNumber, evidence.evidenceNumber)
                    .ok());

    auto reloaded = stack->analysis->findDetection(first.id);
    ASSERT_TRUE(reloaded.ok());
    const auto value = reloaded.take();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->verification, DetectionVerification::Unreviewed);
    EXPECT_EQ(value->reviewMethod, DetectionReviewMethod::NotReviewed)
        << "an unreviewed detection still claims a review method";
    EXPECT_FALSE(value->verifiedBy.has_value());
}

TEST_F(BulkReviewTest, AnUnscopedSweepIsRefused) {
    // Nothing in the interface can produce a filter with no evidence, run or
    // case — which is exactly why it is refused here rather than trusted not to
    // arrive. It would rule on every detection in the database.
    DetectionQuery everything;
    auto swept = stack->analysis->setVerificationForQuery(
        everything, DetectionVerification::Confirmed, "", owner.caseNumber,
        evidence.evidenceNumber, "everything");
    ASSERT_FALSE(swept.ok());
    EXPECT_EQ(swept.error().code(), ErrorCode::InvalidArgument);

    for (const Detection& detection : allDetections()) {
        EXPECT_EQ(detection.verification, DetectionVerification::Unreviewed);
    }
}

TEST_F(BulkReviewTest, ProgressCountsWhatIsLeftAndHowMuchWasIndividual) {
    auto progress = stack->analysis->reviewProgress(run.id);
    ASSERT_TRUE(progress.ok());
    ReviewProgress state = progress.take();
    const std::int64_t total = state.total;
    ASSERT_GT(total, 1);
    EXPECT_EQ(state.unreviewed, total);
    EXPECT_EQ(state.reviewed(), 0);
    EXPECT_DOUBLE_EQ(state.fraction(), 0.0);

    const auto detections = allDetections();
    ASSERT_TRUE(stack->analysis
                    ->setVerification(detections.front(), DetectionVerification::Confirmed, "",
                                      owner.caseNumber, evidence.evidenceNumber)
                    .ok());

    progress = stack->analysis->reviewProgress(run.id);
    ASSERT_TRUE(progress.ok());
    state = progress.take();
    EXPECT_EQ(state.reviewed(), 1);
    EXPECT_EQ(state.confirmed, 1);
    EXPECT_EQ(state.reviewedIndividually, 1);

    ASSERT_TRUE(stack->analysis
                    ->setVerificationForQuery(runQuery(), DetectionVerification::Rejected, "",
                                              owner.caseNumber, evidence.evidenceNumber, "all")
                    .ok());

    progress = stack->analysis->reviewProgress(run.id);
    ASSERT_TRUE(progress.ok());
    state = progress.take();
    EXPECT_EQ(state.total, total);
    EXPECT_EQ(state.reviewed(), total);
    EXPECT_EQ(state.rejected, total);
    EXPECT_DOUBLE_EQ(state.fraction(), 1.0);
    // The individual review was swept over, so nothing is left that a person
    // examined one at a time. A progress indicator that still claimed one would
    // be describing a decision the sweep replaced.
    EXPECT_EQ(state.reviewedIndividually, 0);
}

TEST_F(BulkReviewTest, ReviewsSurviveARestart) {
    ASSERT_TRUE(stack->analysis
                    ->setVerificationForQuery(runQuery(), DetectionVerification::Uncertain, "",
                                              owner.caseNumber, evidence.evidenceNumber, "all")
                    .ok());

    const auto dataRoot = directory->path();
    const std::string runId = run.id;
    const std::string evidenceId = evidence.id;
    stack.reset();

    auto reopened = testing::TestStack::create(dataRoot);
    auto progress = reopened.analysis->reviewProgress(runId);
    ASSERT_TRUE(progress.ok());
    const ReviewProgress state = progress.take();
    EXPECT_GT(state.total, 0);
    EXPECT_EQ(state.uncertain, state.total);
    EXPECT_EQ(state.reviewedIndividually, 0);

    DetectionQuery query;
    query.evidenceId = evidenceId;
    auto detections = reopened.analysis->detections(query);
    ASSERT_TRUE(detections.ok());
    for (const Detection& detection : detections.take()) {
        EXPECT_EQ(detection.reviewMethod, DetectionReviewMethod::Bulk)
            << "the review method did not survive a restart";
    }
}

}  // namespace
}  // namespace trace
