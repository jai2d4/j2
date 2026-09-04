#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/case.h"

namespace trace {

/// Filter for the case browser. Empty fields match everything.
struct CaseQuery {
    std::string text;                       ///< matched against number, title, investigator, agency
    std::optional<CaseStatus> status;
    int limit = 0;                          ///< 0 = unlimited
};

/// Persistence for cases. Repositories own SQL and nothing else: no policy, no
/// audit writes, no filesystem work — services compose those.
class CaseRepository {
public:
    explicit CaseRepository(std::shared_ptr<Database> database);

    Status insert(const Case& value);
    Status update(const Case& value);
    Status remove(const std::string& caseId);

    Result<std::optional<Case>> findById(const std::string& caseId);
    Result<std::optional<Case>> findByCaseNumber(const std::string& caseNumber);
    Result<std::vector<Case>> list(const CaseQuery& query = {});
    Result<std::int64_t> count();

    /// Next unused CASE-nnnn label, derived from existing numbers.
    Result<std::string> nextCaseNumber();

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
