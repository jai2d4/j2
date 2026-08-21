#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/models/case.h"
#include "core/repositories/case_repository.h"
#include "core/services/audit_service.h"
#include "core/storage/storage_layout.h"

namespace trace {

/// Fields an operator supplies when creating a case.
struct CaseDraft {
    std::string caseNumber;
    std::string title;
    std::string description;
    std::string investigator;
    std::string agency;
    std::string location;
    std::string notes;
    CaseStatus status = CaseStatus::Open;
};

/// Case lifecycle: validation, managed storage creation, persistence and audit,
/// in that order, so a case row never exists without its storage directories.
class CaseService {
public:
    CaseService(std::shared_ptr<Database> database, StorageLayout layout,
                std::shared_ptr<AuditService> audit);

    Result<Case> createCase(const CaseDraft& draft);
    /// Loads a case and records that it was opened.
    Result<Case> openCase(const std::string& caseId);
    Status updateCase(const Case& value);

    Result<std::vector<Case>> listCases(const CaseQuery& query = {});
    Result<std::optional<Case>> findById(const std::string& caseId);
    Result<std::optional<Case>> findByCaseNumber(const std::string& caseNumber);
    Result<std::string> suggestNextCaseNumber();
    Result<std::int64_t> countCases();

    const StorageLayout& layout() const { return layout_; }

private:
    std::shared_ptr<CaseRepository> repository_;
    StorageLayout layout_;
    std::shared_ptr<AuditService> audit_;
};

}  // namespace trace
