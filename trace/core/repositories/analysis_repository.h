#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/analysis_run.h"
#include "core/models/detection.h"

namespace trace {

/// Filter for the detections panel, the timeline lanes and the video overlay.
struct DetectionQuery {
    std::optional<std::string> evidenceId;
    std::optional<std::string> analysisRunId;
    std::optional<std::string> caseId;
    std::optional<DetectionClassGroup> classGroup;
    std::optional<DetectionVerification> verification;
    double minimumConfidence = 0.0;
    std::optional<std::int64_t> fromUs;
    std::optional<std::int64_t> toUs;
    /// Rejected detections stay in the record; the panel can hide them from view
    /// without them being deleted.
    bool includeRejected = true;
    int limit = 0;
    int offset = 0;
};

/// How far through reviewing a run an analyst is.
///
/// Counted from the database rather than tracked as the operator works, so it
/// stays right across restarts and across two people reviewing the same run.
struct ReviewProgress {
    std::int64_t total = 0;
    std::int64_t unreviewed = 0;
    std::int64_t confirmed = 0;
    std::int64_t rejected = 0;
    std::int64_t uncertain = 0;
    /// Of the reviewed ones, how many were ruled on individually. The remainder
    /// were part of a bulk decision, which is a weaker claim about what a person
    /// examined and is displayed as such.
    std::int64_t reviewedIndividually = 0;

    std::int64_t reviewed() const { return total - unreviewed; }
    /// 0..1. A run with no detections is complete rather than undefined: there
    /// is nothing left to look at.
    double fraction() const {
        return total == 0 ? 1.0 : static_cast<double>(reviewed()) / static_cast<double>(total);
    }
};

/// One analysed moment at which a class group was present, with how many
/// objects of that group the model reported there.
///
/// The timeline lanes are built from these rather than from whole detection
/// rows: a long recording holds hundreds of thousands of detections but only a
/// few thousand distinct analysed moments, and the lane only needs the moments.
struct DetectionTimelinePoint {
    DetectionClassGroup group = DetectionClassGroup::Object;
    std::int64_t timestampUs = 0;
    std::int64_t count = 0;
};

/// Persistence for analysis runs and the detections they produce.
///
/// Detection inserts are batched: a long recording produces hundreds of
/// thousands of rows, and one statement per row inside its own transaction would
/// dominate the analysis time.
class AnalysisRepository {
public:
    explicit AnalysisRepository(std::shared_ptr<Database> database);

    // ------------------------------------------------------------------ runs
    Status insertRun(const AnalysisRun& run);
    /// Records progress during a run. Deliberately narrow: a running analysis
    /// only ever moves these counters forward.
    Status updateRunProgress(const std::string& runId, std::int64_t framesAnalyzed,
                             std::int64_t detectionsStored,
                             std::optional<std::int64_t> analyzedDurationUs = std::nullopt);
    /// Writes the terminal state. Refuses to mark a run Completed while claiming
    /// an error, so a failed run can never be presented as a successful one.
    Status finishRun(const std::string& runId, AnalysisRunStatus status,
                     const std::string& completedAt,
                     const std::optional<std::string>& errorMessage = std::nullopt,
                     const std::string& warningsJson = "[]");
    Status updateRunStatus(const std::string& runId, AnalysisRunStatus status);
    Status updateRunDevice(const std::string& runId, const std::string& deviceUsed,
                           const std::string& runtime);

    Result<std::optional<AnalysisRun>> findRun(const std::string& runId);
    Result<std::vector<AnalysisRun>> listRunsForEvidence(const std::string& evidenceId);
    Result<std::vector<AnalysisRun>> listRunsForCase(const std::string& caseId);
    /// Most recent run for the item that produced usable results.
    Result<std::optional<AnalysisRun>> latestUsableRun(const std::string& evidenceId);

    // ------------------------------------------------------------ detections
    /// Inserts a batch inside one transaction, reusing a single prepared
    /// statement.
    Status insertDetections(const std::vector<Detection>& detections);

    Result<std::vector<Detection>> listDetections(const DetectionQuery& query);
    Result<std::int64_t> countDetections(const DetectionQuery& query);
    Result<std::optional<Detection>> findDetection(const std::string& detectionId);

    /// Detections belonging to the analysed frame nearest `timestampUs`, within
    /// `toleranceUs`. Returns the whole frame's worth or nothing — never a mix
    /// of two sampled frames, which would draw duplicated boxes.
    Result<std::vector<Detection>> detectionsNearTimestamp(const std::string& evidenceId,
                                                           std::int64_t timestampUs,
                                                           std::int64_t toleranceUs,
                                                           double minimumConfidence = 0.0,
                                                           bool includeRejected = false);

    Status updateVerification(const std::string& detectionId, DetectionVerification state,
                              const std::optional<std::string>& verifiedBy,
                              const std::optional<std::string>& verifiedAt,
                              const std::string& analystNote,
                              DetectionReviewMethod method = DetectionReviewMethod::Individual);

    /// Applies one decision to every detection matching `query`, and returns how
    /// many rows it changed.
    ///
    /// One statement rather than a loop: a filter can match tens of thousands of
    /// rows, and a partially applied sweep would leave the run in a state no
    /// operator asked for and none of them could describe.
    ///
    /// Rows are marked `Bulk` regardless of what they were before. A detection
    /// somebody examined individually and then swept over has, as of the sweep,
    /// last been ruled on by a bulk action — recording otherwise would credit the
    /// current state to an examination that did not produce it.
    Result<std::int64_t> updateVerificationForQuery(const DetectionQuery& query,
                                                    DetectionVerification state,
                                                    const std::optional<std::string>& verifiedBy,
                                                    const std::optional<std::string>& verifiedAt,
                                                    const std::string& analystNote,
                                                    DetectionReviewMethod method);

    /// How much of a run has been ruled on, by state.
    Result<ReviewProgress> reviewProgress(const std::string& analysisRunId);

    /// Distinct class labels present for an item, with counts — drives the filter UI.
    Result<std::vector<std::pair<std::string, std::int64_t>>> classCounts(
        const std::string& evidenceId);

    /// Presence of each class group over time, aggregated per analysed moment.
    /// Honours the same filter as listDetections so the lanes always agree with
    /// the table above them.
    Result<std::vector<DetectionTimelinePoint>> detectionTimeline(const DetectionQuery& query);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
