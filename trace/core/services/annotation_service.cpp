#include "core/services/annotation_service.h"

#include <utility>

#include "core/common/string_utils.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/security/user_context.h"

namespace trace {

AnnotationService::AnnotationService(std::shared_ptr<Database> database,
                                     std::shared_ptr<AuditService> audit)
    : bookmarks_(std::make_shared<BookmarkRepository>(database)),
      annotations_(std::make_shared<AnnotationRepository>(database)),
      audit_(std::move(audit)) {}

Result<Bookmark> AnnotationService::createBookmark(const BookmarkDraft& draft,
                                                   const std::string& caseNumber,
                                                   const std::string& evidenceNumber) {
    using ResultType = Result<Bookmark>;
    if (!UserContext::current().can(Permission::CreateAnnotation)) {
        return ResultType::failure(ErrorCode::PermissionDenied,
                                   "You do not have permission to add bookmarks");
    }
    if (draft.evidenceId.empty() || draft.caseId.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "A bookmark must belong to an evidence item");
    }
    if (draft.timestampUs < 0) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "A bookmark timestamp cannot be negative");
    }

    Bookmark bookmark;
    bookmark.id = generateUuid();
    bookmark.caseId = draft.caseId;
    bookmark.evidenceId = draft.evidenceId;
    bookmark.timestampUs = draft.timestampUs;
    bookmark.frameNumber = draft.frameNumber;
    bookmark.title = trim(draft.title);
    if (bookmark.title.empty()) bookmark.title = "Bookmark at " + formatTimecode(draft.timestampUs);
    bookmark.note = draft.note;
    bookmark.colour = draft.colour;
    bookmark.createdBy = UserContext::current().actorName();
    bookmark.createdAt = nowIso8601Utc();
    bookmark.modifiedAt = bookmark.createdAt;

    if (auto status = bookmarks_->insert(bookmark); !status) return ResultType(status.error());

    AuditRecord record;
    record.action = AuditAction::BookmarkCreated;
    record.caseId = bookmark.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = bookmark.evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.description = "Bookmark created at " + formatTimecode(bookmark.timestampUs) + ": " +
                         bookmark.title;
    record.details = JsonValue::object()
                         .set("bookmark_id", bookmark.id)
                         .set("timestamp_us", bookmark.timestampUs)
                         .set("timecode", formatTimecode(bookmark.timestampUs))
                         .set("title", bookmark.title);
    if (bookmark.frameNumber) record.details.set("frame_number", *bookmark.frameNumber);
    if (auto audited = audit_->record(record); !audited) return ResultType(audited.error());

    return ResultType::success(std::move(bookmark));
}

Status AnnotationService::updateBookmark(const Bookmark& bookmark, const std::string& caseNumber,
                                         const std::string& evidenceNumber) {
    if (!UserContext::current().can(Permission::CreateAnnotation)) {
        return Status::failure(ErrorCode::PermissionDenied,
                               "You do not have permission to edit bookmarks");
    }
    Bookmark updated = bookmark;
    updated.modifiedAt = nowIso8601Utc();
    if (updated.title.empty()) updated.title = "Bookmark at " + formatTimecode(updated.timestampUs);
    if (auto status = bookmarks_->update(updated); !status) return status;

    AuditRecord record;
    record.action = AuditAction::BookmarkUpdated;
    record.caseId = updated.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = updated.evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.description = "Bookmark updated: " + updated.title;
    record.details = JsonValue::object()
                         .set("bookmark_id", updated.id)
                         .set("timestamp_us", updated.timestampUs)
                         .set("timecode", formatTimecode(updated.timestampUs))
                         .set("title", updated.title);
    if (auto audited = audit_->record(record); !audited) return Status(audited.error());
    return Status::success();
}

Status AnnotationService::deleteBookmark(const Bookmark& bookmark, const std::string& caseNumber,
                                         const std::string& evidenceNumber) {
    if (auto status = bookmarks_->remove(bookmark.id); !status) return status;

    AuditRecord record;
    record.action = AuditAction::BookmarkDeleted;
    record.caseId = bookmark.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = bookmark.evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.description = "Bookmark deleted: " + bookmark.title;
    record.details = JsonValue::object()
                         .set("bookmark_id", bookmark.id)
                         .set("timestamp_us", bookmark.timestampUs)
                         .set("title", bookmark.title);
    if (auto audited = audit_->record(record); !audited) return Status(audited.error());
    return Status::success();
}

Result<std::vector<Bookmark>> AnnotationService::bookmarksForEvidence(const std::string& evidenceId) {
    return bookmarks_->listForEvidence(evidenceId);
}

Result<std::vector<Bookmark>> AnnotationService::bookmarksForCase(const std::string& caseId) {
    return bookmarks_->listForCase(caseId);
}

Result<Annotation> AnnotationService::createAnnotation(const AnnotationDraft& draft,
                                                       const std::string& caseNumber,
                                                       const std::string& evidenceNumber) {
    using ResultType = Result<Annotation>;
    if (!UserContext::current().can(Permission::CreateAnnotation)) {
        return ResultType::failure(ErrorCode::PermissionDenied,
                                   "You do not have permission to add annotations");
    }
    if (draft.evidenceId.empty() || draft.caseId.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "An annotation must belong to an evidence item");
    }
    if (trim(draft.text).empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "Annotation text is required");
    }
    if (draft.endUs && *draft.endUs < draft.startUs) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "An annotation cannot end before it starts");
    }

    Annotation annotation;
    annotation.id = generateUuid();
    annotation.caseId = draft.caseId;
    annotation.evidenceId = draft.evidenceId;
    annotation.type = draft.endUs ? AnnotationType::RangeNote : draft.type;
    annotation.startUs = draft.startUs;
    annotation.endUs = draft.endUs;
    annotation.startFrame = draft.startFrame;
    annotation.endFrame = draft.endFrame;
    annotation.text = trim(draft.text);
    annotation.colour = draft.colour;
    annotation.createdBy = UserContext::current().actorName();
    annotation.createdAt = nowIso8601Utc();
    annotation.modifiedAt = annotation.createdAt;

    if (auto status = annotations_->insert(annotation); !status) return ResultType(status.error());

    AuditRecord record;
    record.action = AuditAction::AnnotationCreated;
    record.caseId = annotation.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = annotation.evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.description = "Annotation created at " + formatTimecode(annotation.startUs);
    record.details = JsonValue::object()
                         .set("annotation_id", annotation.id)
                         .set("type", toString(annotation.type))
                         .set("start_us", annotation.startUs)
                         .set("start_timecode", formatTimecode(annotation.startUs))
                         .set("text", annotation.text);
    if (annotation.endUs) {
        record.details.set("end_us", *annotation.endUs)
            .set("end_timecode", formatTimecode(*annotation.endUs));
    }
    if (auto audited = audit_->record(record); !audited) return ResultType(audited.error());

    return ResultType::success(std::move(annotation));
}

Status AnnotationService::updateAnnotation(const Annotation& annotation,
                                           const std::string& caseNumber,
                                           const std::string& evidenceNumber) {
    if (!UserContext::current().can(Permission::CreateAnnotation)) {
        return Status::failure(ErrorCode::PermissionDenied,
                               "You do not have permission to edit annotations");
    }
    if (trim(annotation.text).empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "Annotation text is required");
    }
    Annotation updated = annotation;
    updated.modifiedAt = nowIso8601Utc();
    if (auto status = annotations_->update(updated); !status) return status;

    AuditRecord record;
    record.action = AuditAction::AnnotationUpdated;
    record.caseId = updated.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = updated.evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.description = "Annotation updated at " + formatTimecode(updated.startUs);
    record.details = JsonValue::object()
                         .set("annotation_id", updated.id)
                         .set("start_us", updated.startUs)
                         .set("text", updated.text);
    if (auto audited = audit_->record(record); !audited) return Status(audited.error());
    return Status::success();
}

Status AnnotationService::deleteAnnotation(const Annotation& annotation,
                                           const std::string& caseNumber,
                                           const std::string& evidenceNumber) {
    if (auto status = annotations_->remove(annotation.id); !status) return status;

    AuditRecord record;
    record.action = AuditAction::AnnotationDeleted;
    record.caseId = annotation.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = annotation.evidenceId;
    record.evidenceNumber = evidenceNumber;
    record.description = "Annotation deleted at " + formatTimecode(annotation.startUs);
    record.details = JsonValue::object()
                         .set("annotation_id", annotation.id)
                         .set("start_us", annotation.startUs)
                         .set("text", annotation.text);
    if (auto audited = audit_->record(record); !audited) return Status(audited.error());
    return Status::success();
}

Result<std::vector<Annotation>> AnnotationService::annotationsForEvidence(
    const std::string& evidenceId) {
    return annotations_->listForEvidence(evidenceId);
}

Result<std::vector<Annotation>> AnnotationService::annotationsForCase(const std::string& caseId) {
    return annotations_->listForCase(caseId);
}

}  // namespace trace
