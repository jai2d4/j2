#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/audit_event.h"

namespace trace {

/// Filter for the audit viewer.
struct AuditQuery {
    std::optional<std::string> caseId;
    std::optional<std::string> evidenceId;
    std::optional<AuditAction> action;
    std::string text;      ///< matched against description and actor
    int limit = 500;       ///< 0 = unlimited
    int offset = 0;
    bool newestFirst = true;
};

/// Append-only audit storage.
///
/// There is deliberately no update() or remove(): the database also refuses
/// both through triggers, so a defect in a future UI cannot rewrite history.
class AuditRepository {
public:
    explicit AuditRepository(std::shared_ptr<Database> database);

    /// Assigns the next sequence number and writes the record.
    Result<AuditEvent> append(AuditEvent event);

    Result<std::vector<AuditEvent>> list(const AuditQuery& query = {});
    Result<std::int64_t> count(const AuditQuery& query = {});
    Result<std::int64_t> countForAction(AuditAction action);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
