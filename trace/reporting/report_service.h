#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/models/report.h"
#include "core/services/analysis_service.h"
#include "core/services/annotation_service.h"
#include "core/repositories/report_repository.h"
#include "core/services/audit_service.h"
#include "core/services/case_service.h"
#include "core/services/evidence_service.h"
#include "core/storage/storage_layout.h"
#include "media/export/clip_export_service.h"
#include "media/thumbnails/frame_export_service.h"
#include "reporting/bundle_verifier.h"
#include "reporting/bundle_writer.h"
#include "reporting/report_renderer.h"

namespace trace {

/// What an operator selected before anything has been produced.
struct ReportDraft {
    std::string caseId;
    std::string title;
    std::vector<std::string> evidenceIds;
    std::vector<std::string> detectionIds;
    std::vector<std::string> bookmarkIds;
    std::vector<std::string> annotationIds;

    struct FrameRequest {
        std::string evidenceId;
        Microseconds timestampUs = 0;
        std::string caption;
    };
    struct ClipRequest {
        std::string evidenceId;
        Microseconds startUs = 0;
        Microseconds endUs = 0;
        std::string caption;
    };
    std::vector<FrameRequest> frames;
    std::vector<ClipRequest> clips;

    /// Set only when the operator deliberately chose to include detections no human
    /// has confirmed. Stored on the report and stated in the report itself.
    bool includeUnconfirmedDetections = false;
};

/// Progress while a bundle is being written.
struct ExportProgress {
    int completedSteps = 0;
    int totalSteps = 0;
    std::string stage;
    double fraction() const {
        return totalSteps > 0 ? static_cast<double>(completedSteps) / totalSteps : 0.0;
    }
};

/// Return false to cancel. Called on the exporting thread; keep it cheap.
using ExportProgressCallback = std::function<bool(const ExportProgress&)>;

/// What writing a bundle produced.
struct ExportOutcome {
    Report report;
    ReportStatus status = ReportStatus::Failed;
    std::filesystem::path bundlePath;
    std::string manifestSha256;
    int fileCount = 0;
    /// The bundle re-verified immediately after writing, using the same code a third
    /// party runs. An export that cannot verify its own output is not a success.
    BundleVerification selfCheck;
    std::optional<std::string> error;
};

/// Builds exhibit bundles from a selection, and re-verifies them.
///
/// The service decides what goes in and records what happened; BundleWriter writes
/// bytes and ReportRenderer produces markup. Keeping them apart means the rules about
/// what a report may contain are testable without writing a file.
class ReportService {
public:
    ReportService(StorageLayout layout, std::shared_ptr<Database> database,
                  std::shared_ptr<AuditService> audit, CaseService& cases,
                  EvidenceService& evidence, AnalysisService& analysis,
                  AnnotationService& annotations,
                  std::shared_ptr<DerivedAssetService> derivedAssets);

    /// Records the selection as a draft. Nothing is written to disk yet.
    Result<Report> createReport(const ReportDraft& draft);

    /// Produces the frames and clips, renders the report, writes the bundle and
    /// verifies it. `destinationRoot` is the directory the bundle folder is created
    /// inside; the folder itself is named for the case and the export time.
    Result<ExportOutcome> exportReport(const std::string& reportId,
                                       const std::filesystem::path& destinationRoot,
                                       const ExportProgressCallback& progress = {},
                                       std::shared_ptr<std::atomic<bool>> cancellation = nullptr);

    /// Re-verifies a bundle that already exists, and audits the outcome.
    Result<BundleVerification> verifyBundle(const std::filesystem::path& bundleRoot,
                                            const std::string& caseId = {},
                                            const std::string& caseNumber = {});

    Result<std::vector<Report>> listForCase(const std::string& caseId);
    Result<std::optional<Report>> findReport(const std::string& reportId);

    /// Name of the folder a bundle is written into.
    static std::string bundleFolderName(const std::string& caseNumber,
                                        const std::string& exportedAtUtc);

private:
    StorageLayout layout_;
    std::shared_ptr<ReportRepository> reports_;
    std::shared_ptr<AuditService> audit_;
    CaseService& cases_;
    EvidenceService& evidence_;
    AnalysisService& analysis_;
    AnnotationService& annotations_;
    std::shared_ptr<DerivedAssetService> derivedAssets_;
    ClipExportService clips_;
    FrameExportService frames_;
};

}  // namespace trace
