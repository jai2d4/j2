#include "core/services/analysis_service.h"

#include <utility>

#include "core/common/logging.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/security/user_context.h"

namespace trace {
namespace {

constexpr const char* kComponent = "analysis";

JsonValue warningsToJson(const std::vector<std::string>& warnings) {
    JsonValue array = JsonValue::array();
    for (const auto& warning : warnings) array.push(warning);
    return array;
}

AuditAction auditActionForStatus(AnalysisRunStatus status) {
    switch (status) {
        case AnalysisRunStatus::Completed: return AuditAction::AnalysisCompleted;
        case AnalysisRunStatus::Cancelled: return AuditAction::AnalysisCancelled;
        case AnalysisRunStatus::Failed:    return AuditAction::AnalysisFailed;
        case AnalysisRunStatus::Partial:   return AuditAction::AnalysisCompleted;
        default:                           return AuditAction::AnalysisFailed;
    }
}

AuditAction auditActionForVerification(DetectionVerification state) {
    switch (state) {
        case DetectionVerification::Confirmed: return AuditAction::DetectionConfirmed;
        case DetectionVerification::Rejected:  return AuditAction::DetectionRejected;
        case DetectionVerification::Uncertain: return AuditAction::DetectionMarkedUncertain;
        case DetectionVerification::Unreviewed: break;
    }
    return AuditAction::DetectionMarkedUncertain;
}

}  // namespace

AnalysisService::AnalysisService(std::shared_ptr<Database> database,
                                 std::shared_ptr<AuditService> audit)
    : repository_(std::make_shared<AnalysisRepository>(std::move(database))),
      audit_(std::move(audit)) {}

Result<AnalysisRun> AnalysisService::startRun(const AnalysisRunDraft& draft) {
    using ResultType = Result<AnalysisRun>;

    if (draft.caseId.empty() || draft.evidenceId.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "An analysis run must belong to a case and an evidence item");
    }

    AnalysisRun run;
    run.id = generateUuid();
    run.caseId = draft.caseId;
    run.evidenceId = draft.evidenceId;
    run.analysisType = draft.analysisType;
    run.providerName = draft.providerName;
    run.providerVersion = draft.providerVersion;
    run.runtime = draft.runtime;
    run.modelName = draft.modelName;
    run.modelVersion = draft.modelVersion;
    run.modelSha256 = draft.modelSha256;
    run.modelRelPath = draft.modelRelPath;
    run.deviceRequested = draft.deviceRequested;
    run.configurationJson = draft.configurationJson;
    run.status = AnalysisRunStatus::Running;
    run.startedAt = nowIso8601Utc();
    run.framesExpected = draft.framesExpected;
    run.samplingIntervalUs = draft.samplingIntervalUs;
    run.sourceWidth = draft.sourceWidth;
    run.sourceHeight = draft.sourceHeight;
    run.evidenceSha256 = draft.evidenceSha256;
    run.createdBy = UserContext::current().actorName();
    run.createdAt = run.startedAt.value_or(nowIso8601Utc());

    if (auto status = repository_->insertRun(run); !status) return ResultType(status.error());

    AuditRecord record;
    record.action = AuditAction::AnalysisStarted;
    record.caseId = run.caseId;
    record.caseNumber = draft.caseNumber;
    record.evidenceId = run.evidenceId;
    record.evidenceNumber = draft.evidenceNumber;
    record.description = "Object detection started on " + draft.evidenceNumber;
    record.details = JsonValue::object()
                         .set("analysis_run_id", run.id)
                         .set("analysis_type", run.analysisType)
                         .set("provider", run.providerName)
                         .set("provider_version", run.providerVersion)
                         .set("runtime", run.runtime)
                         .set("model", run.modelName)
                         .set("model_version", run.modelVersion)
                         .set("model_sha256", run.modelSha256.value_or("not applicable"))
                         .set("device_requested", run.deviceRequested)
                         .set("evidence_sha256", run.evidenceSha256);
    if (auto audited = audit_->record(record); !audited) return ResultType(audited.error());

    logInfo(kComponent, "Analysis run started",
            JsonValue::object()
                .set("run", run.id)
                .set("evidence", draft.evidenceNumber)
                .set("model", run.modelName));
    return ResultType::success(std::move(run));
}

Status AnalysisService::recordProgress(const std::string& runId, std::int64_t framesAnalyzed,
                                       std::int64_t detectionsStored,
                                       std::optional<std::int64_t> analyzedDurationUs) {
    return repository_->updateRunProgress(runId, framesAnalyzed, detectionsStored,
                                          analyzedDurationUs);
}

Status AnalysisService::recordDevice(const std::string& runId, const std::string& deviceUsed,
                                     const std::string& runtime) {
    return repository_->updateRunDevice(runId, deviceUsed, runtime);
}

Result<std::int64_t> AnalysisService::storeDetections(const std::vector<Detection>& detections) {
    if (detections.empty()) return Result<std::int64_t>::success(0);
    if (auto status = repository_->insertDetections(detections); !status) {
        return Result<std::int64_t>(status.error());
    }
    return Result<std::int64_t>::success(static_cast<std::int64_t>(detections.size()));
}

Status AnalysisService::finishRun(const AnalysisRun& run, AnalysisRunStatus status,
                                  const std::string& caseNumber,
                                  const std::string& evidenceNumber, std::int64_t framesAnalyzed,
                                  std::int64_t detectionsStored,
                                  const std::optional<std::string>& errorMessage,
                                  const std::vector<std::string>& warnings,
                                  double elapsedSeconds) {
    if (auto progress = repository_->updateRunProgress(run.id, framesAnalyzed, detectionsStored);
        !progress) {
        return progress;
    }
    if (auto finished = repository_->finishRun(run.id, status, nowIso8601Utc(), errorMessage,
                                               warningsToJson(warnings).dump());
        !finished) {
        return finished;
    }

    AuditRecord record;
    record.action = auditActionForStatus(status);
    record.caseId = run.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = run.evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.outcome = status == AnalysisRunStatus::Completed
                         ? "success"
                         : (status == AnalysisRunStatus::Failed ? "failure" : "warning");
    record.description =
        std::string("Object detection ") + toDisplayString(status) + " for " + evidenceNumber +
        ": " + std::to_string(detectionsStored) + " detection(s) across " +
        std::to_string(framesAnalyzed) + " analysed frame(s)";
    record.details = JsonValue::object()
                         .set("analysis_run_id", run.id)
                         .set("status", toString(status))
                         .set("frames_analyzed", framesAnalyzed)
                         .set("detections_stored", detectionsStored)
                         .set("provider", run.providerName)
                         .set("model", run.modelName)
                         .set("model_version", run.modelVersion)
                         .set("model_sha256", run.modelSha256.value_or("not applicable"))
                         .set("elapsed_seconds", elapsedSeconds)
                         .set("evidence_modified", false);
    if (errorMessage) record.details.set("error", *errorMessage);
    if (!warnings.empty()) record.details.set("warnings", warningsToJson(warnings));

    if (auto audited = audit_->record(record); !audited) return Status(audited.error());

    logInfo(kComponent, "Analysis run finished",
            JsonValue::object()
                .set("run", run.id)
                .set("status", toString(status))
                .set("frames", framesAnalyzed)
                .set("detections", detectionsStored));
    return Status::success();
}

Status AnalysisService::recordModelLoaded(const std::string& caseId, const std::string& caseNumber,
                                          const std::string& evidenceId,
                                          const std::string& evidenceNumber,
                                          const std::string& modelName,
                                          const std::string& modelVersion,
                                          const std::optional<std::string>& modelSha256,
                                          const std::string& device, const std::string& runtime) {
    AuditRecord record;
    record.action = AuditAction::ModelLoaded;
    record.caseId = caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.description = "Detection model loaded: " + modelName + " " + modelVersion;
    record.details = JsonValue::object()
                         .set("model", modelName)
                         .set("model_version", modelVersion)
                         .set("model_sha256", modelSha256.value_or("not applicable"))
                         .set("device", device)
                         .set("runtime", runtime);
    auto audited = audit_->record(record);
    if (!audited) return Status(audited.error());
    return Status::success();
}

Status AnalysisService::recordModelValidationFailed(const std::string& caseId,
                                                    const std::string& caseNumber,
                                                    const std::string& evidenceId,
                                                    const std::string& evidenceNumber,
                                                    const std::string& modelName,
                                                    const std::string& problem) {
    AuditRecord record;
    record.action = AuditAction::ModelValidationFailed;
    record.caseId = caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.outcome = "failure";
    record.description = "Detection model rejected: " + modelName;
    record.details = JsonValue::object().set("model", modelName).set("problem", problem);
    auto audited = audit_->record(record);
    if (!audited) return Status(audited.error());
    logError(kComponent, "Model validation failed",
             JsonValue::object().set("model", modelName).set("problem", problem));
    return Status::success();
}

Status AnalysisService::setVerification(const Detection& detection, DetectionVerification state,
                                        const std::string& analystNote,
                                        const std::string& caseNumber,
                                        const std::string& evidenceNumber) {
    if (!UserContext::current().can(Permission::CreateAnnotation)) {
        return Status::failure(ErrorCode::PermissionDenied,
                               "You do not have permission to review detections");
    }

    const std::string actor = UserContext::current().actorName();
    const std::string now = nowIso8601Utc();
    const bool clearing = state == DetectionVerification::Unreviewed;

    if (auto status = repository_->updateVerification(
            detection.id, state, clearing ? std::nullopt : std::optional<std::string>(actor),
            clearing ? std::nullopt : std::optional<std::string>(now), analystNote);
        !status) {
        return status;
    }

    if (clearing) return Status::success();

    AuditRecord record;
    record.action = auditActionForVerification(state);
    record.caseId = detection.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = detection.evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.description = std::string("Detection ") + toDisplayString(state) + ": " +
                         detection.classLabel + " at " + formatTimecode(detection.timestampUs);
    record.details = JsonValue::object()
                         .set("detection_id", detection.id)
                         .set("analysis_run_id", detection.analysisRunId)
                         .set("class", detection.classLabel)
                         .set("confidence", detection.confidence)
                         .set("timestamp_us", detection.timestampUs)
                         .set("timecode", formatTimecode(detection.timestampUs))
                         .set("verification", toString(state))
                         .set("detection_retained", true);
    if (!analystNote.empty()) record.details.set("analyst_note", analystNote);
    auto audited = audit_->record(record);
    if (!audited) return Status(audited.error());
    return Status::success();
}

Result<std::vector<AnalysisRun>> AnalysisService::runsForEvidence(const std::string& evidenceId) {
    return repository_->listRunsForEvidence(evidenceId);
}
Result<std::optional<AnalysisRun>> AnalysisService::latestUsableRun(const std::string& evidenceId) {
    return repository_->latestUsableRun(evidenceId);
}
Result<std::optional<AnalysisRun>> AnalysisService::findRun(const std::string& runId) {
    return repository_->findRun(runId);
}
Result<std::vector<Detection>> AnalysisService::detections(const DetectionQuery& query) {
    return repository_->listDetections(query);
}
Result<std::int64_t> AnalysisService::countDetections(const DetectionQuery& query) {
    return repository_->countDetections(query);
}
Result<std::optional<Detection>> AnalysisService::findDetection(const std::string& detectionId) {
    return repository_->findDetection(detectionId);
}
Result<std::vector<Detection>> AnalysisService::detectionsNearTimestamp(
    const std::string& evidenceId, std::int64_t timestampUs, std::int64_t toleranceUs,
    double minimumConfidence, bool includeRejected) {
    return repository_->detectionsNearTimestamp(evidenceId, timestampUs, toleranceUs,
                                                minimumConfidence, includeRejected);
}
Result<std::vector<std::pair<std::string, std::int64_t>>> AnalysisService::classCounts(
    const std::string& evidenceId) {
    return repository_->classCounts(evidenceId);
}

Result<std::vector<DetectionTimelinePoint>> AnalysisService::detectionTimeline(
    const DetectionQuery& query) {
    return repository_->detectionTimeline(query);
}

}  // namespace trace
