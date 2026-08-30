#include "core/repositories/analysis_repository.h"

#include <utility>

#include "core/common/logging.h"
#include "core/common/string_utils.h"
#include "core/common/time_utils.h"

namespace trace {
namespace {

constexpr const char* kRunColumns =
    "id, case_id, evidence_id, analysis_type, provider_name, provider_version, runtime, "
    "model_name, model_version, model_sha256, model_relpath, device_requested, device_used, "
    "configuration_json, status, started_at, completed_at, frames_analyzed, frames_expected, "
    "detections_stored, analyzed_duration_us, sampling_interval_us, source_width, "
    "source_height, evidence_sha256, error_message, warnings_json, created_by, created_at";

constexpr const char* kDetectionColumns =
    "id, analysis_run_id, case_id, evidence_id, timestamp_us, frame_number, class_id, "
    "class_label, class_group, confidence, bbox_x, bbox_y, bbox_w, bbox_h, bbox_pixel_x, "
    "bbox_pixel_y, bbox_pixel_w, bbox_pixel_h, verification_state, verified_by, verified_at, "
    "analyst_note, created_at, review_method";

AnalysisRun readRun(const Statement& stmt) {
    AnalysisRun run;
    run.id = stmt.columnText(0);
    run.caseId = stmt.columnText(1);
    run.evidenceId = stmt.columnText(2);
    run.analysisType = stmt.columnText(3);
    run.providerName = stmt.columnText(4);
    run.providerVersion = stmt.columnText(5);
    run.runtime = stmt.columnText(6);
    run.modelName = stmt.columnText(7);
    run.modelVersion = stmt.columnText(8);
    run.modelSha256 = stmt.columnOptionalText(9);
    run.modelRelPath = stmt.columnOptionalText(10);
    run.deviceRequested = stmt.columnText(11);
    run.deviceUsed = stmt.columnText(12);
    run.configurationJson = stmt.columnText(13);
    run.status = analysisRunStatusFromString(stmt.columnText(14));
    run.startedAt = stmt.columnOptionalText(15);
    run.completedAt = stmt.columnOptionalText(16);
    run.framesAnalyzed = stmt.columnInt64(17);
    run.framesExpected = stmt.columnOptionalInt64(18);
    run.detectionsStored = stmt.columnInt64(19);
    run.analyzedDurationUs = stmt.columnOptionalInt64(20);
    run.samplingIntervalUs = stmt.columnOptionalInt64(21);
    run.sourceWidth = stmt.columnOptionalInt64(22);
    run.sourceHeight = stmt.columnOptionalInt64(23);
    run.evidenceSha256 = stmt.columnText(24);
    run.errorMessage = stmt.columnOptionalText(25);
    run.warningsJson = stmt.columnText(26);
    run.createdBy = stmt.columnText(27);
    run.createdAt = stmt.columnText(28);
    return run;
}

Detection readDetection(const Statement& stmt) {
    Detection detection;
    detection.id = stmt.columnText(0);
    detection.analysisRunId = stmt.columnText(1);
    detection.caseId = stmt.columnText(2);
    detection.evidenceId = stmt.columnText(3);
    detection.timestampUs = stmt.columnInt64(4);
    detection.frameNumber = stmt.columnOptionalInt64(5);
    detection.classId = stmt.columnInt(6);
    detection.classLabel = stmt.columnText(7);
    detection.classGroup = detectionClassGroupFromString(stmt.columnText(8));
    detection.confidence = stmt.columnDouble(9);
    detection.box.x = stmt.columnDouble(10);
    detection.box.y = stmt.columnDouble(11);
    detection.box.width = stmt.columnDouble(12);
    detection.box.height = stmt.columnDouble(13);
    detection.pixelX = stmt.columnOptionalInt64(14);
    detection.pixelY = stmt.columnOptionalInt64(15);
    detection.pixelWidth = stmt.columnOptionalInt64(16);
    detection.pixelHeight = stmt.columnOptionalInt64(17);
    detection.verification = detectionVerificationFromString(stmt.columnText(18));
    detection.verifiedBy = stmt.columnOptionalText(19);
    detection.verifiedAt = stmt.columnOptionalText(20);
    detection.analystNote = stmt.columnText(21);
    detection.createdAt = stmt.columnText(22);
    // NULL for anything reviewed before migration 0006, and for anything not
    // reviewed at all. Neither is guessed at: the column exists so that "a
    // person looked at this" is a record rather than an inference.
    detection.reviewMethod = detectionReviewMethodFromString(stmt.columnText(23));
    return detection;
}

/// Builds the shared WHERE clause for detection queries. Parameter indices are
/// fixed so list() and count() bind identically.
std::string buildDetectionWhere(const DetectionQuery& query) {
    std::vector<std::string> conditions;
    if (query.evidenceId) conditions.emplace_back("evidence_id = ?1");
    if (query.analysisRunId) conditions.emplace_back("analysis_run_id = ?2");
    if (query.caseId) conditions.emplace_back("case_id = ?3");
    if (query.classGroup) conditions.emplace_back("class_group = ?4");
    if (query.verification) conditions.emplace_back("verification_state = ?5");
    if (query.minimumConfidence > 0.0) conditions.emplace_back("confidence >= ?6");
    if (query.fromUs) conditions.emplace_back("timestamp_us >= ?7");
    if (query.toUs) conditions.emplace_back("timestamp_us <= ?8");
    if (!query.includeRejected) conditions.emplace_back("verification_state <> 'rejected'");
    if (conditions.empty()) return {};
    return " WHERE " + join(conditions, " AND ");
}

void bindDetectionQuery(Statement& stmt, const DetectionQuery& query) {
    if (query.evidenceId) stmt.bind(1, *query.evidenceId);
    if (query.analysisRunId) stmt.bind(2, *query.analysisRunId);
    if (query.caseId) stmt.bind(3, *query.caseId);
    if (query.classGroup) stmt.bind(4, std::string(toString(*query.classGroup)));
    if (query.verification) stmt.bind(5, std::string(toString(*query.verification)));
    if (query.minimumConfidence > 0.0) stmt.bind(6, query.minimumConfidence);
    if (query.fromUs) stmt.bind(7, *query.fromUs);
    if (query.toUs) stmt.bind(8, *query.toUs);
}

}  // namespace

AnalysisRepository::AnalysisRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

// ---------------------------------------------------------------------- runs

Status AnalysisRepository::insertRun(const AnalysisRun& run) {
    auto prepared = database_->prepare(
        std::string("INSERT INTO analysis_runs (") + kRunColumns +
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
        "?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, run.id)
        .bind(2, run.caseId)
        .bind(3, run.evidenceId)
        .bind(4, run.analysisType)
        .bind(5, run.providerName)
        .bind(6, run.providerVersion)
        .bind(7, run.runtime)
        .bind(8, run.modelName)
        .bind(9, run.modelVersion)
        .bindOptional(10, run.modelSha256)
        .bindOptional(11, run.modelRelPath)
        .bind(12, run.deviceRequested)
        .bind(13, run.deviceUsed)
        .bind(14, run.configurationJson)
        .bind(15, std::string(toString(run.status)))
        .bindOptional(16, run.startedAt)
        .bindOptional(17, run.completedAt)
        .bind(18, run.framesAnalyzed)
        .bindOptional(19, run.framesExpected)
        .bind(20, run.detectionsStored)
        .bindOptional(21, run.analyzedDurationUs)
        .bindOptional(22, run.samplingIntervalUs)
        .bindOptional(23, run.sourceWidth)
        .bindOptional(24, run.sourceHeight)
        .bind(25, run.evidenceSha256)
        .bindOptional(26, run.errorMessage)
        .bind(27, run.warningsJson)
        .bind(28, run.createdBy)
        .bind(29, run.createdAt);
    return stmt.run();
}

Status AnalysisRepository::updateRunProgress(const std::string& runId, std::int64_t framesAnalyzed,
                                             std::int64_t detectionsStored,
                                             std::optional<std::int64_t> analyzedDurationUs) {
    auto prepared = database_->prepare(
        "UPDATE analysis_runs SET frames_analyzed = ?, detections_stored = ?, "
        "analyzed_duration_us = COALESCE(?, analyzed_duration_us) WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, framesAnalyzed)
        .bind(2, detectionsStored)
        .bindOptional(3, analyzedDurationUs)
        .bind(4, runId);
    return stmt.run();
}

Status AnalysisRepository::finishRun(const std::string& runId, AnalysisRunStatus status,
                                     const std::string& completedAt,
                                     const std::optional<std::string>& errorMessage,
                                     const std::string& warningsJson) {
    if (status == AnalysisRunStatus::Completed && errorMessage.has_value()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "A run that reported an error cannot be recorded as completed");
    }
    if (!isTerminal(status)) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "finishRun requires a terminal status");
    }

    auto prepared = database_->prepare(
        "UPDATE analysis_runs SET status = ?, completed_at = ?, error_message = ?, "
        "warnings_json = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, std::string(toString(status)))
        .bind(2, completedAt)
        .bindOptional(3, errorMessage)
        .bind(4, warningsJson)
        .bind(5, runId);
    if (auto result = stmt.run(); !result) return result;
    if (database_->changes() == 0) {
        return Status::failure(ErrorCode::NotFound, "Analysis run not found: " + runId);
    }
    return Status::success();
}

Status AnalysisRepository::updateRunStatus(const std::string& runId, AnalysisRunStatus status) {
    auto prepared = database_->prepare(
        "UPDATE analysis_runs SET status = ?, started_at = CASE WHEN ? = 'running' AND "
        "started_at IS NULL THEN ? ELSE started_at END WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    const std::string text = toString(status);
    stmt.bind(1, text).bind(2, text).bind(3, nowIso8601Utc()).bind(4, runId);
    return stmt.run();
}

Status AnalysisRepository::updateRunDevice(const std::string& runId, const std::string& deviceUsed,
                                           const std::string& runtime) {
    auto prepared =
        database_->prepare("UPDATE analysis_runs SET device_used = ?, runtime = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, deviceUsed).bind(2, runtime).bind(3, runId);
    return stmt.run();
}

Result<std::optional<AnalysisRun>> AnalysisRepository::findRun(const std::string& runId) {
    using ResultType = Result<std::optional<AnalysisRun>>;
    auto prepared =
        database_->prepare(std::string("SELECT ") + kRunColumns + " FROM analysis_runs WHERE id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, runId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readRun(stmt));
}

Result<std::vector<AnalysisRun>> AnalysisRepository::listRunsForEvidence(
    const std::string& evidenceId) {
    using ResultType = Result<std::vector<AnalysisRun>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kRunColumns +
                                       " FROM analysis_runs WHERE evidence_id = ? "
                                       "ORDER BY created_at DESC;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);

    std::vector<AnalysisRun> runs;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        runs.push_back(readRun(stmt));
    }
    return ResultType::success(std::move(runs));
}

Result<std::vector<AnalysisRun>> AnalysisRepository::listRunsForCase(const std::string& caseId) {
    using ResultType = Result<std::vector<AnalysisRun>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kRunColumns +
                                       " FROM analysis_runs WHERE case_id = ? ORDER BY created_at DESC;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);

    std::vector<AnalysisRun> runs;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        runs.push_back(readRun(stmt));
    }
    return ResultType::success(std::move(runs));
}

Result<std::optional<AnalysisRun>> AnalysisRepository::latestUsableRun(
    const std::string& evidenceId) {
    using ResultType = Result<std::optional<AnalysisRun>>;
    // Completed first, then partial: both carry real detections, and a partial
    // run is still worth showing as long as it is labelled as partial.
    auto prepared = database_->prepare(
        std::string("SELECT ") + kRunColumns +
        " FROM analysis_runs WHERE evidence_id = ? AND status IN ('completed', 'partial') "
        "ORDER BY CASE status WHEN 'completed' THEN 0 ELSE 1 END, created_at DESC LIMIT 1;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readRun(stmt));
}

// ---------------------------------------------------------------- detections

Status AnalysisRepository::insertDetections(const std::vector<Detection>& detections) {
    if (detections.empty()) return Status::success();

    Transaction transaction(*database_);
    if (auto status = transaction.begin(); !status) return status;

    auto prepared = database_->prepare(
        std::string("INSERT INTO detections (") + kDetectionColumns +
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();

    for (const auto& detection : detections) {
        if (auto status = stmt.reset(); !status) return status;
        stmt.bind(1, detection.id)
            .bind(2, detection.analysisRunId)
            .bind(3, detection.caseId)
            .bind(4, detection.evidenceId)
            .bind(5, detection.timestampUs)
            .bindOptional(6, detection.frameNumber)
            .bind(7, detection.classId)
            .bind(8, detection.classLabel)
            .bind(9, std::string(toString(detection.classGroup)))
            .bind(10, detection.confidence)
            .bind(11, detection.box.x)
            .bind(12, detection.box.y)
            .bind(13, detection.box.width)
            .bind(14, detection.box.height)
            .bindOptional(15, detection.pixelX)
            .bindOptional(16, detection.pixelY)
            .bindOptional(17, detection.pixelWidth)
            .bindOptional(18, detection.pixelHeight)
            .bind(19, std::string(toString(detection.verification)))
            .bindOptional(20, detection.verifiedBy)
            .bindOptional(21, detection.verifiedAt)
            .bind(22, detection.analystNote)
            .bind(23, detection.createdAt)
            // NULL rather than "not_reviewed": an unreviewed detection has no
            // review method, and a string saying so would read as a recorded
            // fact about a review that never happened.
            .bindOptional(24, detection.reviewMethod == DetectionReviewMethod::NotReviewed
                                  ? std::optional<std::string>()
                                  : std::optional<std::string>(toString(detection.reviewMethod)));
        if (auto status = stmt.run(); !status) return status;
    }

    return transaction.commit();
}

Result<std::vector<Detection>> AnalysisRepository::listDetections(const DetectionQuery& query) {
    using ResultType = Result<std::vector<Detection>>;

    std::string sql = std::string("SELECT ") + kDetectionColumns + " FROM detections" +
                      buildDetectionWhere(query) + " ORDER BY timestamp_us, confidence DESC";
    if (query.limit > 0) {
        sql += " LIMIT " + std::to_string(query.limit);
        if (query.offset > 0) sql += " OFFSET " + std::to_string(query.offset);
    }
    sql += ";";

    auto prepared = database_->prepare(sql);
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    bindDetectionQuery(stmt, query);

    std::vector<Detection> detections;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        detections.push_back(readDetection(stmt));
    }
    return ResultType::success(std::move(detections));
}

Result<std::int64_t> AnalysisRepository::countDetections(const DetectionQuery& query) {
    const std::string sql = "SELECT COUNT(*) FROM detections" + buildDetectionWhere(query) + ";";
    auto prepared = database_->prepare(sql);
    if (!prepared) return Result<std::int64_t>(prepared.error());
    Statement stmt = prepared.take();
    bindDetectionQuery(stmt, query);
    auto stepped = stmt.step();
    if (!stepped) return Result<std::int64_t>(stepped.error());
    return Result<std::int64_t>::success(stepped.value() ? stmt.columnInt64(0) : 0);
}

Result<std::optional<Detection>> AnalysisRepository::findDetection(const std::string& detectionId) {
    using ResultType = Result<std::optional<Detection>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kDetectionColumns +
                                       " FROM detections WHERE id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, detectionId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readDetection(stmt));
}

Result<std::vector<Detection>> AnalysisRepository::detectionsNearTimestamp(
    const std::string& evidenceId, std::int64_t timestampUs, std::int64_t toleranceUs,
    double minimumConfidence, bool includeRejected) {
    using ResultType = Result<std::vector<Detection>>;

    auto guard = database_->lockGuard();

    // Find the analysed frame closest to the playhead...
    auto nearest = database_->prepare(
        "SELECT timestamp_us FROM detections WHERE evidence_id = ? AND timestamp_us >= ? "
        "AND timestamp_us <= ? ORDER BY ABS(timestamp_us - ?) LIMIT 1;");
    if (!nearest) return ResultType(nearest.error());
    Statement nearestStmt = nearest.take();
    nearestStmt.bind(1, evidenceId)
        .bind(2, timestampUs - toleranceUs)
        .bind(3, timestampUs + toleranceUs)
        .bind(4, timestampUs);
    auto stepped = nearestStmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::vector<Detection>{});
    const std::int64_t frameTimestamp = nearestStmt.columnInt64(0);

    // ...then take that frame's detections exactly, so boxes from two sampled
    // frames are never drawn on top of each other.
    DetectionQuery query;
    query.evidenceId = evidenceId;
    query.fromUs = frameTimestamp;
    query.toUs = frameTimestamp;
    query.minimumConfidence = minimumConfidence;
    query.includeRejected = includeRejected;
    return listDetections(query);
}

Status AnalysisRepository::updateVerification(const std::string& detectionId,
                                              DetectionVerification state,
                                              const std::optional<std::string>& verifiedBy,
                                              const std::optional<std::string>& verifiedAt,
                                              const std::string& analystNote,
                                              DetectionReviewMethod method) {
    auto prepared = database_->prepare(
        "UPDATE detections SET verification_state = ?, verified_by = ?, verified_at = ?, "
        "analyst_note = ?, review_method = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    // Clearing a review clears how it was made: leaving the method behind would
    // describe a decision that no longer exists.
    const bool clearing = state == DetectionVerification::Unreviewed;
    stmt.bind(1, std::string(toString(state)))
        .bindOptional(2, verifiedBy)
        .bindOptional(3, verifiedAt)
        .bind(4, analystNote)
        .bindOptional(5, clearing || method == DetectionReviewMethod::NotReviewed
                             ? std::optional<std::string>()
                             : std::optional<std::string>(toString(method)))
        .bind(6, detectionId);
    if (auto status = stmt.run(); !status) return status;
    if (database_->changes() == 0) {
        return Status::failure(ErrorCode::NotFound, "Detection not found: " + detectionId);
    }
    return Status::success();
}

Result<std::int64_t> AnalysisRepository::updateVerificationForQuery(
    const DetectionQuery& query, DetectionVerification state,
    const std::optional<std::string>& verifiedBy, const std::optional<std::string>& verifiedAt,
    const std::string& analystNote, DetectionReviewMethod method) {
    using ResultType = Result<std::int64_t>;

    // A sweep with no filter at all would rule on every detection in the
    // database, across every case. Nothing in the UI can produce that, which is
    // exactly why it is refused here rather than trusted not to happen.
    if (!query.evidenceId && !query.analysisRunId && !query.caseId) {
        return ResultType::failure(
            ErrorCode::InvalidArgument,
            "A bulk review must name the evidence, the run or the case it applies to");
    }

    // The WHERE clause and its bindings are shared with list() and count(), so
    // the rows changed are exactly the rows the operator was shown a count of.
    // SET parameters start at 10 to stay clear of the filter's fixed 1..8.
    const bool clearing = state == DetectionVerification::Unreviewed;
    auto prepared = database_->prepare(
        "UPDATE detections SET verification_state = ?10, verified_by = ?11, verified_at = ?12, "
        "analyst_note = ?13, review_method = ?14" +
        buildDetectionWhere(query) + ";");
    if (!prepared) return ResultType(prepared.error());

    Statement stmt = prepared.take();
    bindDetectionQuery(stmt, query);
    stmt.bind(10, std::string(toString(state)))
        .bindOptional(11, verifiedBy)
        .bindOptional(12, verifiedAt)
        .bind(13, analystNote)
        .bindOptional(14, clearing || method == DetectionReviewMethod::NotReviewed
                              ? std::optional<std::string>()
                              : std::optional<std::string>(toString(method)));
    if (auto status = stmt.run(); !status) return ResultType(status.error());
    return ResultType::success(database_->changes());
}

Result<ReviewProgress> AnalysisRepository::reviewProgress(const std::string& analysisRunId) {
    using ResultType = Result<ReviewProgress>;
    auto prepared = database_->prepare(
        "SELECT verification_state, review_method, count(*) FROM detections "
        "WHERE analysis_run_id = ? GROUP BY verification_state, review_method;");
    if (!prepared) return ResultType(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, analysisRunId);

    ReviewProgress progress;
    while (true) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.take()) break;

        const auto state = detectionVerificationFromString(stmt.columnText(0));
        const auto method = detectionReviewMethodFromString(stmt.columnText(1));
        const std::int64_t count = stmt.columnInt64(2);

        progress.total += count;
        switch (state) {
            case DetectionVerification::Unreviewed: progress.unreviewed += count; break;
            case DetectionVerification::Confirmed:  progress.confirmed += count; break;
            case DetectionVerification::Rejected:   progress.rejected += count; break;
            case DetectionVerification::Uncertain:  progress.uncertain += count; break;
        }
        if (state != DetectionVerification::Unreviewed &&
            method == DetectionReviewMethod::Individual) {
            progress.reviewedIndividually += count;
        }
    }
    return ResultType::success(progress);
}

Result<std::vector<DetectionTimelinePoint>> AnalysisRepository::detectionTimeline(
    const DetectionQuery& query) {
    using ResultType = Result<std::vector<DetectionTimelinePoint>>;

    // Grouping in SQL keeps this cheap on a long recording: the result has one
    // row per analysed moment per class group, not one per detection.
    const std::string sql = "SELECT class_group, timestamp_us, COUNT(*) FROM detections" +
                            buildDetectionWhere(query) +
                            " GROUP BY class_group, timestamp_us ORDER BY class_group, "
                            "timestamp_us;";
    auto prepared = database_->prepare(sql);
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    bindDetectionQuery(stmt, query);

    std::vector<DetectionTimelinePoint> points;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        DetectionTimelinePoint point;
        point.group = detectionClassGroupFromString(stmt.columnText(0));
        point.timestampUs = stmt.columnInt64(1);
        point.count = stmt.columnInt64(2);
        points.push_back(point);
    }
    return ResultType::success(std::move(points));
}

Result<std::vector<std::pair<std::string, std::int64_t>>> AnalysisRepository::classCounts(
    const std::string& evidenceId) {
    using ResultType = Result<std::vector<std::pair<std::string, std::int64_t>>>;
    auto prepared = database_->prepare(
        "SELECT class_label, COUNT(*) FROM detections WHERE evidence_id = ? "
        "GROUP BY class_label ORDER BY COUNT(*) DESC;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);

    std::vector<std::pair<std::string, std::int64_t>> counts;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        counts.emplace_back(stmt.columnText(0), stmt.columnInt64(1));
    }
    return ResultType::success(std::move(counts));
}

}  // namespace trace
