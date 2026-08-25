// String conversions for the domain enums. The persisted forms are stable:
// changing one is a schema-visible change and needs a migration.
#include <algorithm>

#include "core/models/annotation.h"
#include "core/models/audit_event.h"
#include "core/models/case.h"
#include "core/models/analysis_run.h"
#include "core/models/derived_asset.h"
#include "core/models/detection.h"
#include "core/models/report.h"
#include "core/models/evidence.h"
#include "core/models/media_metadata.h"

namespace trace {

// ----------------------------------------------------------------- CaseStatus

const char* toString(CaseStatus status) {
    switch (status) {
        case CaseStatus::Open:     return "open";
        case CaseStatus::Active:   return "active";
        case CaseStatus::OnHold:   return "on_hold";
        case CaseStatus::Closed:   return "closed";
        case CaseStatus::Archived: return "archived";
    }
    return "open";
}

const char* toDisplayString(CaseStatus status) {
    switch (status) {
        case CaseStatus::Open:     return "Open";
        case CaseStatus::Active:   return "Active";
        case CaseStatus::OnHold:   return "On Hold";
        case CaseStatus::Closed:   return "Closed";
        case CaseStatus::Archived: return "Archived";
    }
    return "Open";
}

CaseStatus caseStatusFromString(const std::string& text, CaseStatus fallback) {
    if (text == "open") return CaseStatus::Open;
    if (text == "active") return CaseStatus::Active;
    if (text == "on_hold") return CaseStatus::OnHold;
    if (text == "closed") return CaseStatus::Closed;
    if (text == "archived") return CaseStatus::Archived;
    return fallback;
}

// ------------------------------------------------------------------ MediaType

const char* toString(MediaType type) {
    switch (type) {
        case MediaType::Unknown:  return "unknown";
        case MediaType::Video:    return "video";
        case MediaType::Audio:    return "audio";
        case MediaType::Image:    return "image";
        case MediaType::Document: return "document";
        case MediaType::Other:    return "other";
    }
    return "unknown";
}

const char* toDisplayString(MediaType type) {
    switch (type) {
        case MediaType::Unknown:  return "Unknown";
        case MediaType::Video:    return "Video";
        case MediaType::Audio:    return "Audio";
        case MediaType::Image:    return "Image";
        case MediaType::Document: return "Document";
        case MediaType::Other:    return "Other";
    }
    return "Unknown";
}

MediaType mediaTypeFromString(const std::string& text, MediaType fallback) {
    if (text == "video") return MediaType::Video;
    if (text == "audio") return MediaType::Audio;
    if (text == "image") return MediaType::Image;
    if (text == "document") return MediaType::Document;
    if (text == "other") return MediaType::Other;
    if (text == "unknown") return MediaType::Unknown;
    return fallback;
}

// ------------------------------------------------------------ IntegrityStatus

const char* toString(IntegrityStatus status) {
    switch (status) {
        case IntegrityStatus::NotVerified: return "not_verified";
        case IntegrityStatus::Verified:    return "verified";
        case IntegrityStatus::Failed:      return "failed";
        case IntegrityStatus::Verifying:   return "verifying";
        case IntegrityStatus::Error:       return "error";
    }
    return "not_verified";
}

const char* toDisplayString(IntegrityStatus status) {
    switch (status) {
        case IntegrityStatus::NotVerified: return "NOT VERIFIED";
        case IntegrityStatus::Verified:    return "VERIFIED";
        case IntegrityStatus::Failed:      return "INTEGRITY FAILURE";
        case IntegrityStatus::Verifying:   return "VERIFYING";
        case IntegrityStatus::Error:       return "ERROR";
    }
    return "NOT VERIFIED";
}

IntegrityStatus integrityStatusFromString(const std::string& text, IntegrityStatus fallback) {
    if (text == "not_verified") return IntegrityStatus::NotVerified;
    if (text == "verified") return IntegrityStatus::Verified;
    if (text == "failed") return IntegrityStatus::Failed;
    if (text == "verifying") return IntegrityStatus::Verifying;
    if (text == "error") return IntegrityStatus::Error;
    return fallback;
}

// ---------------------------------------------------------------- MediaStatus

const char* toString(MediaStatus status) {
    switch (status) {
        case MediaStatus::Unknown:        return "unknown";
        case MediaStatus::Decodable:      return "decodable";
        case MediaStatus::MetadataFailed: return "metadata_failed";
        case MediaStatus::Unsupported:    return "unsupported";
        case MediaStatus::NotMedia:       return "not_media";
    }
    return "unknown";
}

const char* toDisplayString(MediaStatus status) {
    switch (status) {
        case MediaStatus::Unknown:        return "Unknown";
        case MediaStatus::Decodable:      return "Decodable";
        case MediaStatus::MetadataFailed: return "Metadata unreadable";
        case MediaStatus::Unsupported:    return "Codec unsupported";
        case MediaStatus::NotMedia:       return "Not a media file";
    }
    return "Unknown";
}

MediaStatus mediaStatusFromString(const std::string& text, MediaStatus fallback) {
    if (text == "decodable") return MediaStatus::Decodable;
    if (text == "metadata_failed") return MediaStatus::MetadataFailed;
    if (text == "unsupported") return MediaStatus::Unsupported;
    if (text == "not_media") return MediaStatus::NotMedia;
    if (text == "unknown") return MediaStatus::Unknown;
    return fallback;
}

// -------------------------------------------------------------- FrameRateMode

const char* toString(FrameRateMode mode) {
    switch (mode) {
        case FrameRateMode::Unknown:           return "unknown";
        case FrameRateMode::ConstantSuspected: return "constant_suspected";
        case FrameRateMode::Variable:          return "variable";
    }
    return "unknown";
}

const char* toDisplayString(FrameRateMode mode) {
    switch (mode) {
        case FrameRateMode::Unknown:           return "Undetermined";
        case FrameRateMode::ConstantSuspected: return "Constant (reported)";
        case FrameRateMode::Variable:          return "Variable";
    }
    return "Undetermined";
}

FrameRateMode frameRateModeFromString(const std::string& text, FrameRateMode fallback) {
    if (text == "constant_suspected") return FrameRateMode::ConstantSuspected;
    if (text == "variable") return FrameRateMode::Variable;
    if (text == "unknown") return FrameRateMode::Unknown;
    return fallback;
}

// ----------------------------------------------------------------- StreamType

const char* toString(StreamType type) {
    switch (type) {
        case StreamType::Video:      return "video";
        case StreamType::Audio:      return "audio";
        case StreamType::Subtitle:   return "subtitle";
        case StreamType::Data:       return "data";
        case StreamType::Attachment: return "attachment";
        case StreamType::Unknown:    return "unknown";
    }
    return "unknown";
}

StreamType streamTypeFromString(const std::string& text) {
    if (text == "video") return StreamType::Video;
    if (text == "audio") return StreamType::Audio;
    if (text == "subtitle") return StreamType::Subtitle;
    if (text == "data") return StreamType::Data;
    if (text == "attachment") return StreamType::Attachment;
    return StreamType::Unknown;
}

// --------------------------------------------------- MetadataExtractionStatus

const char* toString(MetadataExtractionStatus status) {
    switch (status) {
        case MetadataExtractionStatus::Unknown: return "unknown";
        case MetadataExtractionStatus::Ok:      return "ok";
        case MetadataExtractionStatus::Partial: return "partial";
        case MetadataExtractionStatus::Failed:  return "failed";
    }
    return "unknown";
}

const char* toDisplayString(MetadataExtractionStatus status) {
    switch (status) {
        case MetadataExtractionStatus::Unknown: return "Not attempted";
        case MetadataExtractionStatus::Ok:      return "Extracted";
        case MetadataExtractionStatus::Partial: return "Partially extracted";
        case MetadataExtractionStatus::Failed:  return "Extraction failed";
    }
    return "Not attempted";
}

MetadataExtractionStatus metadataExtractionStatusFromString(const std::string& text,
                                                            MetadataExtractionStatus fallback) {
    if (text == "ok") return MetadataExtractionStatus::Ok;
    if (text == "partial") return MetadataExtractionStatus::Partial;
    if (text == "failed") return MetadataExtractionStatus::Failed;
    if (text == "unknown") return MetadataExtractionStatus::Unknown;
    return fallback;
}

// ------------------------------------------------------------- AnnotationType

const char* toString(AnnotationType type) {
    switch (type) {
        case AnnotationType::TimestampNote: return "timestamp_note";
        case AnnotationType::RangeNote:     return "range_note";
        case AnnotationType::Point:         return "point";
        case AnnotationType::Region:        return "region";
        case AnnotationType::Arrow:         return "arrow";
        case AnnotationType::Measurement:   return "measurement";
    }
    return "timestamp_note";
}

const char* toDisplayString(AnnotationType type) {
    switch (type) {
        case AnnotationType::TimestampNote: return "Timestamp note";
        case AnnotationType::RangeNote:     return "Range note";
        case AnnotationType::Point:         return "Point";
        case AnnotationType::Region:        return "Region";
        case AnnotationType::Arrow:         return "Arrow";
        case AnnotationType::Measurement:   return "Measurement";
    }
    return "Timestamp note";
}

AnnotationType annotationTypeFromString(const std::string& text, AnnotationType fallback) {
    if (text == "timestamp_note") return AnnotationType::TimestampNote;
    if (text == "range_note") return AnnotationType::RangeNote;
    if (text == "point") return AnnotationType::Point;
    if (text == "region") return AnnotationType::Region;
    if (text == "arrow") return AnnotationType::Arrow;
    if (text == "measurement") return AnnotationType::Measurement;
    return fallback;
}

// ----------------------------------------------------------- DerivedAssetType

const char* toString(DerivedAssetType type) {
    switch (type) {
        case DerivedAssetType::FrameExport:    return "frame_export";
        case DerivedAssetType::Thumbnail:      return "thumbnail";
        case DerivedAssetType::WorkingCopy:    return "working_copy";
        case DerivedAssetType::Clip:           return "clip";
        case DerivedAssetType::AudioExtract:   return "audio_extract";
        case DerivedAssetType::Waveform:       return "waveform";
        case DerivedAssetType::AnalysisOutput: return "analysis_output";
        case DerivedAssetType::Report:         return "report";
        case DerivedAssetType::Other:          return "other";
    }
    return "other";
}

const char* toDisplayString(DerivedAssetType type) {
    switch (type) {
        case DerivedAssetType::FrameExport:    return "Extracted frame";
        case DerivedAssetType::Thumbnail:      return "Thumbnail";
        case DerivedAssetType::WorkingCopy:    return "Working copy";
        case DerivedAssetType::Clip:           return "Clip";
        case DerivedAssetType::AudioExtract:   return "Audio extract";
        case DerivedAssetType::Waveform:       return "Waveform";
        case DerivedAssetType::AnalysisOutput: return "Analysis output";
        case DerivedAssetType::Report:         return "Report";
        case DerivedAssetType::Other:          return "Other";
    }
    return "Other";
}

DerivedAssetType derivedAssetTypeFromString(const std::string& text, DerivedAssetType fallback) {
    if (text == "frame_export") return DerivedAssetType::FrameExport;
    if (text == "thumbnail") return DerivedAssetType::Thumbnail;
    if (text == "working_copy") return DerivedAssetType::WorkingCopy;
    if (text == "clip") return DerivedAssetType::Clip;
    if (text == "audio_extract") return DerivedAssetType::AudioExtract;
    if (text == "waveform") return DerivedAssetType::Waveform;
    if (text == "analysis_output") return DerivedAssetType::AnalysisOutput;
    if (text == "report") return DerivedAssetType::Report;
    if (text == "other") return DerivedAssetType::Other;
    return fallback;
}

// ---------------------------------------------------------- VerificationState

const char* toString(VerificationState state) {
    switch (state) {
        case VerificationState::Unverified:    return "unverified";
        case VerificationState::HumanVerified: return "human_verified";
        case VerificationState::Rejected:      return "rejected";
    }
    return "unverified";
}

const char* toDisplayString(VerificationState state) {
    switch (state) {
        case VerificationState::Unverified:    return "Not reviewed";
        case VerificationState::HumanVerified: return "Analyst verified";
        case VerificationState::Rejected:      return "Rejected";
    }
    return "Not reviewed";
}

VerificationState verificationStateFromString(const std::string& text, VerificationState fallback) {
    if (text == "unverified") return VerificationState::Unverified;
    if (text == "human_verified") return VerificationState::HumanVerified;
    if (text == "rejected") return VerificationState::Rejected;
    return fallback;
}

// ---------------------------------------------------------------- AuditAction

const char* toString(AuditAction action) {
    switch (action) {
        case AuditAction::ApplicationStarted:        return "application.started";
        case AuditAction::ApplicationStopped:        return "application.stopped";
        case AuditAction::DatabaseMigrated:          return "database.migrated";
        case AuditAction::CaseCreated:               return "case.created";
        case AuditAction::CaseOpened:                return "case.opened";
        case AuditAction::CaseUpdated:               return "case.updated";
        case AuditAction::CaseStatusChanged:         return "case.status_changed";
        case AuditAction::EvidenceImportStarted:     return "evidence.import_started";
        case AuditAction::EvidenceImported:          return "evidence.imported";
        case AuditAction::EvidenceImportFailed:      return "evidence.import_failed";
        case AuditAction::EvidenceHashed:            return "evidence.hashed";
        case AuditAction::EvidenceMetadataExtracted: return "evidence.metadata_extracted";
        case AuditAction::EvidenceMetadataFailed:    return "evidence.metadata_failed";
        case AuditAction::EvidenceViewed:            return "evidence.viewed";
        case AuditAction::IntegrityVerified:         return "integrity.verified";
        case AuditAction::IntegrityFailed:           return "integrity.failed";
        case AuditAction::BookmarkCreated:           return "bookmark.created";
        case AuditAction::BookmarkUpdated:           return "bookmark.updated";
        case AuditAction::BookmarkDeleted:           return "bookmark.deleted";
        case AuditAction::AnnotationCreated:         return "annotation.created";
        case AuditAction::AnnotationUpdated:         return "annotation.updated";
        case AuditAction::AnnotationDeleted:         return "annotation.deleted";
        case AuditAction::FrameExtracted:            return "frame.extracted";
        case AuditAction::DerivedAssetCreated:       return "derived_asset.created";
        case AuditAction::ExportCreated:             return "export.created";
        case AuditAction::ClipExported:              return "clip.exported";
        case AuditAction::ReportCreated:             return "report.created";
        case AuditAction::ReportExported:            return "report.exported";
        case AuditAction::ReportExportFailed:        return "report.export_failed";
        case AuditAction::ReportExportCancelled:     return "report.export_cancelled";
        case AuditAction::BundleVerified:            return "report.bundle_verified";
        case AuditAction::BundleVerificationFailed:  return "report.bundle_verification_failed";
        case AuditAction::SettingsChanged:           return "settings.changed";
        case AuditAction::AnalysisStarted:           return "analysis.started";
        case AuditAction::AnalysisCompleted:         return "analysis.completed";
        case AuditAction::AnalysisCancelled:         return "analysis.cancelled";
        case AuditAction::AnalysisFailed:            return "analysis.failed";
        case AuditAction::AnalysisResultsExported:   return "analysis.results_exported";
        case AuditAction::DetectionConfirmed:        return "detection.confirmed";
        case AuditAction::DetectionRejected:         return "detection.rejected";
        case AuditAction::DetectionMarkedUncertain:  return "detection.marked_uncertain";
        case AuditAction::ModelLoaded:               return "model.loaded";
        case AuditAction::ModelValidationFailed:     return "model.validation_failed";
        case AuditAction::SignInSucceeded:         return "auth.sign_in_succeeded";
        case AuditAction::SignInFailed:            return "auth.sign_in_failed";
        case AuditAction::SignedOut:               return "auth.signed_out";
        case AuditAction::AccountLocked:           return "auth.account_locked";
        case AuditAction::AccountCreated:          return "auth.account_created";
        case AuditAction::AccountRoleChanged:      return "auth.role_changed";
        case AuditAction::AccountDeactivated:      return "auth.account_deactivated";
        case AuditAction::PasswordChanged:         return "auth.password_changed";
        case AuditAction::PasswordReset:           return "auth.password_reset";
        case AuditAction::Unknown:                   return "unknown";
    }
    return "unknown";
}

const char* toDisplayString(AuditAction action) {
    switch (action) {
        case AuditAction::ApplicationStarted:        return "Application started";
        case AuditAction::ApplicationStopped:        return "Application stopped";
        case AuditAction::DatabaseMigrated:          return "Database migrated";
        case AuditAction::CaseCreated:               return "Case created";
        case AuditAction::CaseOpened:                return "Case opened";
        case AuditAction::CaseUpdated:               return "Case updated";
        case AuditAction::CaseStatusChanged:         return "Case status changed";
        case AuditAction::EvidenceImportStarted:     return "Evidence import started";
        case AuditAction::EvidenceImported:          return "Evidence imported";
        case AuditAction::EvidenceImportFailed:      return "Evidence import failed";
        case AuditAction::EvidenceHashed:            return "Evidence hashed";
        case AuditAction::EvidenceMetadataExtracted: return "Metadata extracted";
        case AuditAction::EvidenceMetadataFailed:    return "Metadata extraction failed";
        case AuditAction::EvidenceViewed:            return "Evidence viewed";
        case AuditAction::IntegrityVerified:         return "Integrity verified";
        case AuditAction::IntegrityFailed:           return "Integrity verification failed";
        case AuditAction::BookmarkCreated:           return "Bookmark created";
        case AuditAction::BookmarkUpdated:           return "Bookmark updated";
        case AuditAction::BookmarkDeleted:           return "Bookmark deleted";
        case AuditAction::AnnotationCreated:         return "Annotation created";
        case AuditAction::AnnotationUpdated:         return "Annotation updated";
        case AuditAction::AnnotationDeleted:         return "Annotation deleted";
        case AuditAction::FrameExtracted:            return "Frame extracted";
        case AuditAction::DerivedAssetCreated:       return "Derived asset created";
        case AuditAction::ExportCreated:             return "Export created";
        case AuditAction::ClipExported:              return "Clip exported";
        case AuditAction::ReportCreated:             return "Report created";
        case AuditAction::ReportExported:            return "Report exported";
        case AuditAction::ReportExportFailed:        return "Report export failed";
        case AuditAction::ReportExportCancelled:     return "Report export cancelled";
        case AuditAction::BundleVerified:            return "Exhibit bundle verified";
        case AuditAction::BundleVerificationFailed:  return "Exhibit bundle verification failed";
        case AuditAction::SettingsChanged:           return "Settings changed";
        case AuditAction::AnalysisStarted:           return "Analysis started";
        case AuditAction::AnalysisCompleted:         return "Analysis completed";
        case AuditAction::AnalysisCancelled:         return "Analysis cancelled";
        case AuditAction::AnalysisFailed:            return "Analysis failed";
        case AuditAction::AnalysisResultsExported:   return "Analysis results exported";
        case AuditAction::DetectionConfirmed:        return "Detection confirmed by analyst";
        case AuditAction::DetectionRejected:         return "Detection rejected by analyst";
        case AuditAction::DetectionMarkedUncertain:  return "Detection marked uncertain";
        case AuditAction::ModelLoaded:               return "Model loaded";
        case AuditAction::ModelValidationFailed:     return "Model validation failed";
        case AuditAction::SignInSucceeded:         return "Sign-in succeeded";
        case AuditAction::SignInFailed:            return "Sign-in failed";
        case AuditAction::SignedOut:               return "Signed out";
        case AuditAction::AccountLocked:           return "Account locked";
        case AuditAction::AccountCreated:          return "Account created";
        case AuditAction::AccountRoleChanged:      return "Account role changed";
        case AuditAction::AccountDeactivated:      return "Account deactivated";
        case AuditAction::PasswordChanged:         return "Password changed";
        case AuditAction::PasswordReset:           return "Password reset";
        case AuditAction::Unknown:                   return "Unknown action";
    }
    return "Unknown action";
}

AuditAction auditActionFromString(const std::string& text) {
    static const struct { const char* text; AuditAction action; } kMap[] = {
        {"application.started", AuditAction::ApplicationStarted},
        {"application.stopped", AuditAction::ApplicationStopped},
        {"database.migrated", AuditAction::DatabaseMigrated},
        {"case.created", AuditAction::CaseCreated},
        {"case.opened", AuditAction::CaseOpened},
        {"case.updated", AuditAction::CaseUpdated},
        {"case.status_changed", AuditAction::CaseStatusChanged},
        {"evidence.import_started", AuditAction::EvidenceImportStarted},
        {"evidence.imported", AuditAction::EvidenceImported},
        {"evidence.import_failed", AuditAction::EvidenceImportFailed},
        {"evidence.hashed", AuditAction::EvidenceHashed},
        {"evidence.metadata_extracted", AuditAction::EvidenceMetadataExtracted},
        {"evidence.metadata_failed", AuditAction::EvidenceMetadataFailed},
        {"evidence.viewed", AuditAction::EvidenceViewed},
        {"integrity.verified", AuditAction::IntegrityVerified},
        {"integrity.failed", AuditAction::IntegrityFailed},
        {"bookmark.created", AuditAction::BookmarkCreated},
        {"bookmark.updated", AuditAction::BookmarkUpdated},
        {"bookmark.deleted", AuditAction::BookmarkDeleted},
        {"annotation.created", AuditAction::AnnotationCreated},
        {"annotation.updated", AuditAction::AnnotationUpdated},
        {"annotation.deleted", AuditAction::AnnotationDeleted},
        {"frame.extracted", AuditAction::FrameExtracted},
        {"derived_asset.created", AuditAction::DerivedAssetCreated},
        {"export.created", AuditAction::ExportCreated},
        {"clip.exported", AuditAction::ClipExported},
        {"report.created", AuditAction::ReportCreated},
        {"report.exported", AuditAction::ReportExported},
        {"report.export_failed", AuditAction::ReportExportFailed},
        {"report.export_cancelled", AuditAction::ReportExportCancelled},
        {"report.bundle_verified", AuditAction::BundleVerified},
        {"report.bundle_verification_failed", AuditAction::BundleVerificationFailed},
        {"settings.changed", AuditAction::SettingsChanged},
        {"analysis.started", AuditAction::AnalysisStarted},
        {"analysis.completed", AuditAction::AnalysisCompleted},
        {"analysis.cancelled", AuditAction::AnalysisCancelled},
        {"analysis.failed", AuditAction::AnalysisFailed},
        {"analysis.results_exported", AuditAction::AnalysisResultsExported},
        {"detection.confirmed", AuditAction::DetectionConfirmed},
        {"detection.rejected", AuditAction::DetectionRejected},
        {"detection.marked_uncertain", AuditAction::DetectionMarkedUncertain},
        {"model.loaded", AuditAction::ModelLoaded},
        {"model.validation_failed", AuditAction::ModelValidationFailed},
        {"auth.sign_in_succeeded", AuditAction::SignInSucceeded},
        {"auth.sign_in_failed", AuditAction::SignInFailed},
        {"auth.signed_out", AuditAction::SignedOut},
        {"auth.account_locked", AuditAction::AccountLocked},
        {"auth.account_created", AuditAction::AccountCreated},
        {"auth.role_changed", AuditAction::AccountRoleChanged},
        {"auth.account_deactivated", AuditAction::AccountDeactivated},
        {"auth.password_changed", AuditAction::PasswordChanged},
        {"auth.password_reset", AuditAction::PasswordReset},
    };
    for (const auto& entry : kMap) {
        if (text == entry.text) return entry.action;
    }
    return AuditAction::Unknown;
}

// ------------------------------------------------------------ AnalysisRunStatus

const char* toString(AnalysisRunStatus status) {
    switch (status) {
        case AnalysisRunStatus::Queued:    return "queued";
        case AnalysisRunStatus::Running:   return "running";
        case AnalysisRunStatus::Paused:    return "paused";
        case AnalysisRunStatus::Completed: return "completed";
        case AnalysisRunStatus::Cancelled: return "cancelled";
        case AnalysisRunStatus::Failed:    return "failed";
        case AnalysisRunStatus::Partial:   return "partial";
    }
    return "queued";
}

const char* toDisplayString(AnalysisRunStatus status) {
    switch (status) {
        case AnalysisRunStatus::Queued:    return "QUEUED";
        case AnalysisRunStatus::Running:   return "RUNNING";
        case AnalysisRunStatus::Paused:    return "PAUSED";
        case AnalysisRunStatus::Completed: return "COMPLETED";
        case AnalysisRunStatus::Cancelled: return "CANCELLED";
        case AnalysisRunStatus::Failed:    return "FAILED";
        case AnalysisRunStatus::Partial:   return "PARTIAL";
    }
    return "QUEUED";
}

AnalysisRunStatus analysisRunStatusFromString(const std::string& text, AnalysisRunStatus fallback) {
    if (text == "queued") return AnalysisRunStatus::Queued;
    if (text == "running") return AnalysisRunStatus::Running;
    if (text == "paused") return AnalysisRunStatus::Paused;
    if (text == "completed") return AnalysisRunStatus::Completed;
    if (text == "cancelled") return AnalysisRunStatus::Cancelled;
    if (text == "failed") return AnalysisRunStatus::Failed;
    if (text == "partial") return AnalysisRunStatus::Partial;
    return fallback;
}

bool isTerminal(AnalysisRunStatus status) {
    switch (status) {
        case AnalysisRunStatus::Completed:
        case AnalysisRunStatus::Cancelled:
        case AnalysisRunStatus::Failed:
        case AnalysisRunStatus::Partial:
            return true;
        case AnalysisRunStatus::Queued:
        case AnalysisRunStatus::Running:
        case AnalysisRunStatus::Paused:
            return false;
    }
    return false;
}

bool producedCompleteResults(AnalysisRunStatus status) {
    return status == AnalysisRunStatus::Completed;
}

// --------------------------------------------------------- DetectionClassGroup

const char* toString(DetectionClassGroup group) {
    switch (group) {
        case DetectionClassGroup::Person:  return "person";
        case DetectionClassGroup::Vehicle: return "vehicle";
        case DetectionClassGroup::Object:  return "object";
    }
    return "object";
}

const char* toDisplayString(DetectionClassGroup group) {
    switch (group) {
        case DetectionClassGroup::Person:  return "People";
        case DetectionClassGroup::Vehicle: return "Vehicles";
        case DetectionClassGroup::Object:  return "Objects";
    }
    return "Objects";
}

DetectionClassGroup detectionClassGroupFromString(const std::string& text,
                                                  DetectionClassGroup fallback) {
    if (text == "person") return DetectionClassGroup::Person;
    if (text == "vehicle") return DetectionClassGroup::Vehicle;
    if (text == "object") return DetectionClassGroup::Object;
    return fallback;
}

// -------------------------------------------------------- DetectionVerification

const char* toString(DetectionVerification state) {
    switch (state) {
        case DetectionVerification::Unreviewed: return "unreviewed";
        case DetectionVerification::Confirmed:  return "confirmed";
        case DetectionVerification::Rejected:   return "rejected";
        case DetectionVerification::Uncertain:  return "uncertain";
    }
    return "unreviewed";
}

const char* toDisplayString(DetectionVerification state) {
    switch (state) {
        case DetectionVerification::Unreviewed: return "UNREVIEWED";
        case DetectionVerification::Confirmed:  return "CONFIRMED";
        case DetectionVerification::Rejected:   return "REJECTED";
        case DetectionVerification::Uncertain:  return "UNCERTAIN";
    }
    return "UNREVIEWED";
}

DetectionVerification detectionVerificationFromString(const std::string& text,
                                                      DetectionVerification fallback) {
    if (text == "unreviewed") return DetectionVerification::Unreviewed;
    if (text == "confirmed") return DetectionVerification::Confirmed;
    if (text == "rejected") return DetectionVerification::Rejected;
    if (text == "uncertain") return DetectionVerification::Uncertain;
    return fallback;
}

// ---------------------------------------------------------------- NormalizedBox

NormalizedBox NormalizedBox::clampedToFrame() const {
    const double left = std::clamp(x, 0.0, 1.0);
    const double top = std::clamp(y, 0.0, 1.0);
    const double right = std::clamp(x + width, 0.0, 1.0);
    const double bottom = std::clamp(y + height, 0.0, 1.0);
    NormalizedBox clamped;
    clamped.x = left;
    clamped.y = top;
    clamped.width = right > left ? right - left : 0.0;
    clamped.height = bottom > top ? bottom - top : 0.0;
    return clamped;
}


// ------------------------------------------------------------------- reports

const char* toString(ReportStatus status) {
    switch (status) {
        case ReportStatus::Draft: return "draft";
        case ReportStatus::Exporting: return "exporting";
        case ReportStatus::Exported: return "exported";
        case ReportStatus::Cancelled: return "cancelled";
        case ReportStatus::Failed: return "failed";
    }
    return "draft";
}

const char* toDisplayString(ReportStatus status) {
    switch (status) {
        case ReportStatus::Draft: return "Draft";
        case ReportStatus::Exporting: return "Exporting";
        case ReportStatus::Exported: return "Exported";
        case ReportStatus::Cancelled: return "Cancelled";
        case ReportStatus::Failed: return "Failed";
    }
    return "Draft";
}

ReportStatus reportStatusFromString(const std::string& text, ReportStatus fallback) {
    if (text == "draft") return ReportStatus::Draft;
    if (text == "exporting") return ReportStatus::Exporting;
    if (text == "exported") return ReportStatus::Exported;
    if (text == "cancelled") return ReportStatus::Cancelled;
    if (text == "failed") return ReportStatus::Failed;
    return fallback;
}

bool isTerminal(ReportStatus status) {
    return status == ReportStatus::Exported || status == ReportStatus::Cancelled ||
           status == ReportStatus::Failed;
}

bool producedCompleteBundle(ReportStatus status) { return status == ReportStatus::Exported; }

const char* toString(ReportItemType type) {
    switch (type) {
        case ReportItemType::Evidence: return "evidence";
        case ReportItemType::Detection: return "detection";
        case ReportItemType::Frame: return "frame";
        case ReportItemType::Clip: return "clip";
        case ReportItemType::Bookmark: return "bookmark";
        case ReportItemType::Annotation: return "annotation";
    }
    return "evidence";
}

const char* toDisplayString(ReportItemType type) {
    switch (type) {
        case ReportItemType::Evidence: return "Evidence";
        case ReportItemType::Detection: return "Detection";
        case ReportItemType::Frame: return "Frame";
        case ReportItemType::Clip: return "Clip";
        case ReportItemType::Bookmark: return "Bookmark";
        case ReportItemType::Annotation: return "Note";
    }
    return "Evidence";
}

ReportItemType reportItemTypeFromString(const std::string& text, ReportItemType fallback) {
    if (text == "evidence") return ReportItemType::Evidence;
    if (text == "detection") return ReportItemType::Detection;
    if (text == "frame") return ReportItemType::Frame;
    if (text == "clip") return ReportItemType::Clip;
    if (text == "bookmark") return ReportItemType::Bookmark;
    if (text == "annotation") return ReportItemType::Annotation;
    return fallback;
}

}  // namespace trace
