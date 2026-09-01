#pragma once

#include <optional>
#include <string>

namespace trace {

/// Actions recorded in the append-only audit trail.
///
/// The enum is the vocabulary the application uses; the database stores the
/// stable string form, so adding actions later never invalidates old records.
enum class AuditAction {
    ApplicationStarted,
    ApplicationStopped,
    DatabaseMigrated,
    CaseCreated,
    CaseOpened,
    CaseUpdated,
    CaseStatusChanged,
    EvidenceImportStarted,
    EvidenceImported,
    EvidenceImportFailed,
    EvidenceHashed,
    EvidenceMetadataExtracted,
    EvidenceMetadataFailed,
    EvidenceViewed,
    IntegrityVerified,
    IntegrityFailed,
    BookmarkCreated,
    BookmarkUpdated,
    BookmarkDeleted,
    AnnotationCreated,
    AnnotationUpdated,
    AnnotationDeleted,
    FrameExtracted,
    DerivedAssetCreated,
    ExportCreated,
    ClipExported,
    ReportCreated,
    ReportExported,
    ReportExportFailed,
    ReportExportCancelled,
    BundleVerified,
    BundleVerificationFailed,
    SettingsChanged,
    // Phase 1 — analysis
    AnalysisStarted,
    AnalysisCompleted,
    AnalysisCancelled,
    AnalysisFailed,
    AnalysisResultsExported,
    DetectionConfirmed,
    DetectionRejected,
    DetectionMarkedUncertain,
    ModelLoaded,
    ModelValidationFailed,
    // Authentication. A failed sign-in is recorded as deliberately as a
    // successful one: a run of failures against one account is the signal that
    // somebody is trying, and it is exactly what a trail like this is for.
    SignInSucceeded,
    SignInFailed,
    SignedOut,
    AccountLocked,
    AccountCreated,
    AccountRoleChanged,
    AccountDeactivated,
    PasswordChanged,
    PasswordReset,
    // Live capture. Deliberately separate from the Evidence* actions above:
    // those record a file arriving from outside, where the chain of custody
    // began before TRACE. A capture has no such origin — TRACE is the recording
    // device — and the trail must not read as though a pre-existing file was
    // imported.
    CaptureStarted,
    CaptureCompleted,
    CaptureFailed,
    CaptureInterrupted,   ///< the link dropped mid-recording; a gap was recorded
    CameraDiscovered,
    Unknown,
};

const char* toString(AuditAction action);
const char* toDisplayString(AuditAction action);
AuditAction auditActionFromString(const std::string& text);

/// One immutable audit record.
///
/// `sequence` is a monotonically increasing counter within the database, so
/// records keep their order even when two events share a millisecond.
struct AuditEvent {
    std::string id;
    std::int64_t sequence = 0;
    std::string occurredAt;
    AuditAction action = AuditAction::Unknown;
    std::string actor;
    std::optional<std::string> actorUserId;
    std::optional<std::string> caseId;
    std::optional<std::string> caseNumber;
    std::optional<std::string> evidenceId;
    std::optional<std::string> evidenceNumber;
    std::string description;
    std::string detailsJson = "{}";
    std::string outcome = "success";  ///< success | failure | warning
    std::string appVersion;
    std::string host;
};

}  // namespace trace
