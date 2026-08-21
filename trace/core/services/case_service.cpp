#include "core/services/case_service.h"

#include <utility>

#include "core/common/logging.h"
#include "core/common/string_utils.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/security/user_context.h"

namespace trace {
namespace {
constexpr const char* kComponent = "case";
}

CaseService::CaseService(std::shared_ptr<Database> database, StorageLayout layout,
                         std::shared_ptr<AuditService> audit)
    : repository_(std::make_shared<CaseRepository>(std::move(database))),
      layout_(std::move(layout)),
      audit_(std::move(audit)) {}

Result<Case> CaseService::createCase(const CaseDraft& draft) {
    using ResultType = Result<Case>;

    if (!UserContext::current().can(Permission::CreateCase)) {
        return ResultType::failure(ErrorCode::PermissionDenied,
                                   "You do not have permission to create cases");
    }

    Case value;
    value.caseNumber = trim(draft.caseNumber);
    value.title = trim(draft.title);
    if (value.caseNumber.empty()) {
        auto suggested = suggestNextCaseNumber();
        if (!suggested) return ResultType(suggested.error());
        value.caseNumber = suggested.take();
    }
    if (value.title.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "A case title is required");
    }

    auto existing = repository_->findByCaseNumber(value.caseNumber);
    if (!existing) return ResultType(existing.error());
    if (existing.value().has_value()) {
        return ResultType::failure(ErrorCode::AlreadyExists,
                                   "Case number already in use: " + value.caseNumber);
    }

    value.id = generateUuid();
    value.description = draft.description;
    value.status = draft.status;
    value.investigator = trim(draft.investigator);
    value.agency = trim(draft.agency);
    value.location = trim(draft.location);
    value.notes = draft.notes;
    value.createdAt = nowIso8601Utc();
    value.modifiedAt = value.createdAt;
    value.createdBy = UserContext::current().actorName();
    value.storageRelPath = StorageLayout::caseRelPath(value.id);

    // Storage first: a case row that points at directories which do not exist
    // is worse than no case at all.
    if (auto status = layout_.ensureCaseDirectories(value.id); !status) {
        return ResultType(status.error());
    }
    if (auto status = repository_->insert(value); !status) {
        return ResultType(status.error());
    }

    logInfo(kComponent, "Case created",
            JsonValue::object().set("case_number", value.caseNumber).set("case_id", value.id));

    AuditRecord record;
    record.action = AuditAction::CaseCreated;
    record.caseId = value.id;
    record.caseNumber = value.caseNumber;
    record.description = "Case " + value.caseNumber + " created: " + value.title;
    record.details = JsonValue::object()
                         .set("title", value.title)
                         .set("status", toString(value.status))
                         .setIfNotEmpty("investigator", value.investigator)
                         .setIfNotEmpty("agency", value.agency)
                         .setIfNotEmpty("location", value.location)
                         .set("storage_relpath", value.storageRelPath);
    if (auto audited = audit_->record(record); !audited) return ResultType(audited.error());

    return ResultType::success(std::move(value));
}

Result<Case> CaseService::openCase(const std::string& caseId) {
    using ResultType = Result<Case>;

    auto found = repository_->findById(caseId);
    if (!found) return ResultType(found.error());
    if (!found.value().has_value()) {
        return ResultType::failure(ErrorCode::NotFound, "Case not found");
    }
    Case value = *found.take();

    // Directories may be missing if the data directory was restored partially.
    if (auto status = layout_.ensureCaseDirectories(value.id); !status) {
        logWarn(kComponent, "Unable to ensure case directories",
                JsonValue::object().set("case_id", value.id).set("detail", status.error().detail()));
    }

    AuditRecord record;
    record.action = AuditAction::CaseOpened;
    record.caseId = value.id;
    record.caseNumber = value.caseNumber;
    record.description = "Case " + value.caseNumber + " opened";
    if (auto audited = audit_->record(record); !audited) return ResultType(audited.error());

    return ResultType::success(std::move(value));
}

Status CaseService::updateCase(const Case& value) {
    if (!UserContext::current().can(Permission::EditCase)) {
        return Status::failure(ErrorCode::PermissionDenied,
                               "You do not have permission to edit cases");
    }
    if (trim(value.title).empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "A case title is required");
    }

    auto existing = repository_->findById(value.id);
    if (!existing) return Status(existing.error());
    if (!existing.value().has_value()) {
        return Status::failure(ErrorCode::NotFound, "Case not found");
    }
    const Case previous = *existing.take();

    Case updated = value;
    updated.modifiedAt = nowIso8601Utc();
    if (auto status = repository_->update(updated); !status) return status;

    JsonValue details = JsonValue::object();
    if (previous.title != updated.title) {
        details.set("title", JsonValue::object().set("from", previous.title).set("to", updated.title));
    }
    if (previous.status != updated.status) {
        details.set("status", JsonValue::object()
                                  .set("from", toString(previous.status))
                                  .set("to", toString(updated.status)));
    }
    if (previous.investigator != updated.investigator) {
        details.set("investigator", JsonValue::object()
                                        .set("from", previous.investigator)
                                        .set("to", updated.investigator));
    }

    AuditRecord record;
    record.action = previous.status != updated.status ? AuditAction::CaseStatusChanged
                                                      : AuditAction::CaseUpdated;
    record.caseId = updated.id;
    record.caseNumber = updated.caseNumber;
    record.description = "Case " + updated.caseNumber + " updated";
    record.details = details;
    if (auto audited = audit_->record(record); !audited) return Status(audited.error());

    return Status::success();
}

Result<std::vector<Case>> CaseService::listCases(const CaseQuery& query) {
    return repository_->list(query);
}

Result<std::optional<Case>> CaseService::findById(const std::string& caseId) {
    return repository_->findById(caseId);
}

Result<std::optional<Case>> CaseService::findByCaseNumber(const std::string& caseNumber) {
    return repository_->findByCaseNumber(caseNumber);
}

Result<std::string> CaseService::suggestNextCaseNumber() { return repository_->nextCaseNumber(); }

Result<std::int64_t> CaseService::countCases() { return repository_->count(); }

}  // namespace trace
