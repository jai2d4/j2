#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trace {

/// Lifecycle of a report.
///
/// A report that failed to export is never Exported. Cancelled and Failed are distinct
/// and both honest: the operator stopped it, or it could not be written.
enum class ReportStatus { Draft, Exporting, Exported, Cancelled, Failed };

const char* toString(ReportStatus status);
const char* toDisplayString(ReportStatus status);
ReportStatus reportStatusFromString(const std::string& text,
                                    ReportStatus fallback = ReportStatus::Draft);
/// True when the report reached a state it will not leave.
bool isTerminal(ReportStatus status);
/// True when a bundle was written completely and can be cited.
bool producedCompleteBundle(ReportStatus status);

/// What kind of thing an operator put in a report.
enum class ReportItemType { Evidence, Detection, Frame, Clip, Bookmark, Annotation };

const char* toString(ReportItemType type);
const char* toDisplayString(ReportItemType type);
ReportItemType reportItemTypeFromString(const std::string& text,
                                        ReportItemType fallback = ReportItemType::Evidence);

/// One thing the operator chose to include.
///
/// Which identifier is set depends on `type`. A frame or clip additionally carries
/// `derivedAssetId` once the file has actually been produced — before that it is a
/// request, not an artefact.
struct ReportItem {
    std::string id;
    std::string reportId;
    ReportItemType type = ReportItemType::Evidence;

    std::optional<std::string> evidenceId;
    std::optional<std::string> detectionId;
    std::optional<std::string> bookmarkId;
    std::optional<std::string> annotationId;
    std::optional<std::string> derivedAssetId;

    std::optional<std::int64_t> timestampUs;    ///< frames
    std::optional<std::int64_t> rangeStartUs;   ///< clips
    std::optional<std::int64_t> rangeEndUs;

    std::string caption;
    int sortOrder = 0;
    std::string createdAt;
};

/// A selection of case material, and the record of writing it out.
///
/// The report is the provenance anchor for a bundle: it names the software version that
/// produced it, when the export ran, what it contained, and the digest of the manifest
/// that indexes every file. A bundle whose manifest digest matches this row is the
/// bundle this report produced.
struct Report {
    std::string id;
    std::string caseId;
    std::string title;

    ReportStatus status = ReportStatus::Draft;
    std::optional<std::string> bundleRelPath;
    std::optional<std::string> manifestSha256;
    std::string traceVersion;

    std::optional<std::string> exportStartedAt;
    std::optional<std::string> exportCompletedAt;
    std::string itemCountsJson = "{}";
    /// True when the operator deliberately included detections no human confirmed.
    bool includedUnconfirmed = false;

    std::optional<std::string> errorMessage;
    std::string createdBy;
    std::string createdAt;

    std::vector<ReportItem> items;
};

}  // namespace trace
