#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/models/analysis_run.h"
#include "core/models/detection.h"
#include "core/repositories/analysis_repository.h"
#include "core/services/audit_service.h"

namespace trace {

/// What an analysis run needs to know about itself before it starts.
struct AnalysisRunDraft {
    std::string caseId;
    std::string caseNumber;
    std::string evidenceId;
    std::string evidenceNumber;
    std::string analysisType = "object_detection";

    std::string providerName;
    std::string providerVersion;
    std::string runtime;
    std::string modelName;
    std::string modelVersion;
    std::optional<std::string> modelSha256;
    std::optional<std::string> modelRelPath;

    std::string deviceRequested = "auto";
    std::string configurationJson = "{}";
    std::optional<std::int64_t> framesExpected;
    std::optional<std::int64_t> samplingIntervalUs;
    std::optional<std::int64_t> sourceWidth;
    std::optional<std::int64_t> sourceHeight;
    /// Digest the evidence carried when the run began.
    std::string evidenceSha256;
};

/// Persistence and audit policy for analysis runs and their detections.
///
/// The pipeline executes; this decides what gets written and what gets recorded
/// in the audit trail. Keeping the two apart means the storage rules are
/// testable without running a model.
class AnalysisService {
public:
    AnalysisService(std::shared_ptr<Database> database, std::shared_ptr<AuditService> audit);

    /// Creates the run in Running state and records analysis.started.
    Result<AnalysisRun> startRun(const AnalysisRunDraft& draft);

    Status recordProgress(const std::string& runId, std::int64_t framesAnalyzed,
                          std::int64_t detectionsStored,
                          std::optional<std::int64_t> analyzedDurationUs = std::nullopt);
    Status recordDevice(const std::string& runId, const std::string& deviceUsed,
                        const std::string& runtime);

    /// Writes a batch of detections. Returns how many rows were stored.
    Result<std::int64_t> storeDetections(const std::vector<Detection>& detections);

    /// Records the terminal state and the matching audit event. A run that
    /// carries an error is never recorded as completed.
    Status finishRun(const AnalysisRun& run, AnalysisRunStatus status,
                     const std::string& caseNumber, const std::string& evidenceNumber,
                     std::int64_t framesAnalyzed, std::int64_t detectionsStored,
                     const std::optional<std::string>& errorMessage = std::nullopt,
                     const std::vector<std::string>& warnings = {},
                     double elapsedSeconds = 0.0);

    /// Records that a model artefact was loaded, with the digest of the exact
    /// file, or that validation refused it.
    Status recordModelLoaded(const std::string& caseId, const std::string& caseNumber,
                             const std::string& evidenceId, const std::string& evidenceNumber,
                             const std::string& modelName, const std::string& modelVersion,
                             const std::optional<std::string>& modelSha256,
                             const std::string& device, const std::string& runtime);
    Status recordModelValidationFailed(const std::string& caseId, const std::string& caseNumber,
                                       const std::string& evidenceId,
                                       const std::string& evidenceNumber,
                                       const std::string& modelName, const std::string& problem);

    /// Analyst review. Rejected detections are kept: they are part of the
    /// analytical history, and the decision itself is audited.
    Status setVerification(const Detection& detection, DetectionVerification state,
                           const std::string& analystNote, const std::string& caseNumber,
                           const std::string& evidenceNumber);

    // Queries used by the panels.
    Result<std::vector<AnalysisRun>> runsForEvidence(const std::string& evidenceId);
    Result<std::optional<AnalysisRun>> latestUsableRun(const std::string& evidenceId);
    Result<std::optional<AnalysisRun>> findRun(const std::string& runId);
    Result<std::vector<Detection>> detections(const DetectionQuery& query);
    Result<std::int64_t> countDetections(const DetectionQuery& query);
    Result<std::optional<Detection>> findDetection(const std::string& detectionId);
    Result<std::vector<Detection>> detectionsNearTimestamp(const std::string& evidenceId,
                                                           std::int64_t timestampUs,
                                                           std::int64_t toleranceUs,
                                                           double minimumConfidence,
                                                           bool includeRejected);
    Result<std::vector<std::pair<std::string, std::int64_t>>> classCounts(
        const std::string& evidenceId);
    Result<std::vector<DetectionTimelinePoint>> detectionTimeline(const DetectionQuery& query);

private:
    std::shared_ptr<AnalysisRepository> repository_;
    std::shared_ptr<AuditService> audit_;
};

}  // namespace trace
