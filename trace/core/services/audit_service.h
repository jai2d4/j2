#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/json.h"
#include "core/common/result.h"
#include "core/models/audit_event.h"
#include "core/repositories/audit_repository.h"

namespace trace {

/// What the caller knows about an action; the service fills in identity, time,
/// host and application version.
struct AuditRecord {
    AuditAction action = AuditAction::Unknown;
    std::optional<std::string> caseId;
    std::optional<std::string> caseNumber;
    std::optional<std::string> evidenceId;
    std::optional<std::string> evidenceNumber;
    std::string description;
    JsonValue details = JsonValue::object();
    std::string outcome = "success";  ///< success | failure | warning
};

/// Writes the append-only audit trail.
///
/// Audit failures are logged and returned, never swallowed: if TRACE cannot
/// record what it did, the operator needs to know.
class AuditService {
public:
    explicit AuditService(std::shared_ptr<Database> database);

    Result<AuditEvent> record(const AuditRecord& record);
    Result<std::vector<AuditEvent>> list(const AuditQuery& query = {});
    Result<std::int64_t> count(const AuditQuery& query = {});

    /// Hostname recorded alongside each event.
    static std::string hostName();

private:
    std::shared_ptr<AuditRepository> repository_;
};

}  // namespace trace
