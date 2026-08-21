#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/models/annotation.h"
#include "core/models/bookmark.h"
#include "core/models/evidence.h"
#include "core/repositories/annotation_repository.h"
#include "core/repositories/bookmark_repository.h"
#include "core/services/audit_service.h"

namespace trace {

struct BookmarkDraft {
    std::string evidenceId;
    std::string caseId;
    std::int64_t timestampUs = 0;
    std::optional<std::int64_t> frameNumber;
    std::string title;
    std::string note;
    std::optional<std::string> colour;
};

struct AnnotationDraft {
    std::string evidenceId;
    std::string caseId;
    AnnotationType type = AnnotationType::TimestampNote;
    std::int64_t startUs = 0;
    std::optional<std::int64_t> endUs;
    std::optional<std::int64_t> startFrame;
    std::optional<std::int64_t> endFrame;
    std::string text;
    std::optional<std::string> colour;
};

/// Bookmarks and annotations, with audit records for every change.
class AnnotationService {
public:
    AnnotationService(std::shared_ptr<Database> database, std::shared_ptr<AuditService> audit);

    Result<Bookmark> createBookmark(const BookmarkDraft& draft, const std::string& caseNumber,
                                    const std::string& evidenceNumber);
    Status updateBookmark(const Bookmark& bookmark, const std::string& caseNumber,
                          const std::string& evidenceNumber);
    Status deleteBookmark(const Bookmark& bookmark, const std::string& caseNumber,
                          const std::string& evidenceNumber);
    Result<std::vector<Bookmark>> bookmarksForEvidence(const std::string& evidenceId);
    Result<std::vector<Bookmark>> bookmarksForCase(const std::string& caseId);

    Result<Annotation> createAnnotation(const AnnotationDraft& draft, const std::string& caseNumber,
                                        const std::string& evidenceNumber);
    Status updateAnnotation(const Annotation& annotation, const std::string& caseNumber,
                            const std::string& evidenceNumber);
    Status deleteAnnotation(const Annotation& annotation, const std::string& caseNumber,
                            const std::string& evidenceNumber);
    Result<std::vector<Annotation>> annotationsForEvidence(const std::string& evidenceId);
    Result<std::vector<Annotation>> annotationsForCase(const std::string& caseId);

private:
    std::shared_ptr<BookmarkRepository> bookmarks_;
    std::shared_ptr<AnnotationRepository> annotations_;
    std::shared_ptr<AuditService> audit_;
};

}  // namespace trace
