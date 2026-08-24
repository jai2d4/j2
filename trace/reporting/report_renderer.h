#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/models/analysis_run.h"
#include "core/models/annotation.h"
#include "core/models/audit_event.h"
#include "core/models/bookmark.h"
#include "core/models/case.h"
#include "core/models/detection.h"
#include "core/models/evidence.h"
#include "core/models/media_metadata.h"
#include "core/models/report.h"
#include "reporting/bundle_writer.h"

namespace trace {

/// One frame or clip that was produced and placed in the bundle.
struct ExhibitReference {
    ReportItemType type = ReportItemType::Frame;
    std::string evidenceNumber;
    std::string fileName;   ///< inside exhibits/
    std::string sha256;
    std::int64_t timestampUs = 0;
    std::optional<std::int64_t> rangeStartUs;
    std::optional<std::int64_t> rangeEndUs;
    /// For a stream-copied clip, where the extraction actually began. Recorded
    /// separately from the request because a clip must start on a keyframe.
    std::optional<std::int64_t> actualStartUs;
    std::string caption;
    bool reencoded = false;
};

/// One detection cited, joined to the run that produced it.
struct CitedDetection {
    Detection detection;
    AnalysisRun run;
    std::string evidenceNumber;
};

/// Everything the report is rendered from. Assembled by the service; the renderer
/// itself reads records and writes markup, and decides nothing.
struct ReportContent {
    Case caseRecord;
    Report report;
    std::string exportedAtUtc;
    std::string exportedBy;
    std::string traceVersion;

    std::vector<Evidence> evidence;
    std::vector<std::optional<MediaMetadata>> evidenceMetadata;  ///< parallel to `evidence`
    std::vector<CitedDetection> detections;
    std::vector<ExhibitReference> exhibits;
    std::vector<Bookmark> bookmarks;
    std::vector<Annotation> annotations;
    std::vector<AuditEvent> auditEvents;
};

/// Renders the report and the machine-readable records that travel with it.
///
/// Deterministic: the same content produces byte-identical output apart from the export
/// timestamp. No Qt, no network, no template engine — a bundle must render offline
/// forever, so the HTML carries its own styling and references only files beside it.
class ReportRenderer {
public:
    static std::string renderHtml(const ReportContent& content);

    static std::string renderEvidenceJson(const ReportContent& content);
    static std::string renderAnalysisRunsJson(const ReportContent& content);
    static std::string renderDetectionsJson(const ReportContent& content);
    static std::string renderAuditJson(const ReportContent& content);
    static std::string renderAuditCsv(const ReportContent& content);
};

}  // namespace trace
