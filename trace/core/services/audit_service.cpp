#include "core/services/audit_service.h"

#include <utility>

#include "core/common/logging.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/security/user_context.h"
#include "trace/trace_version.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <unistd.h>
#include <climits>
#endif

namespace trace {
namespace {
constexpr const char* kComponent = "audit";
}

AuditService::AuditService(std::shared_ptr<Database> database)
    : repository_(std::make_shared<AuditRepository>(std::move(database))) {}

std::string AuditService::hostName() {
    static const std::string host = [] {
#if defined(_WIN32)
        char buffer[256] = {};
        DWORD size = sizeof(buffer);
        if (GetComputerNameA(buffer, &size) != 0) return std::string(buffer, size);
        return std::string("unknown-host");
#else
        char buffer[HOST_NAME_MAX + 1] = {};
        if (gethostname(buffer, sizeof(buffer) - 1) == 0) return std::string(buffer);
        return std::string("unknown-host");
#endif
    }();
    return host;
}

Result<AuditEvent> AuditService::record(const AuditRecord& record) {
    AuditEvent event;
    event.id = generateUuid();
    event.occurredAt = nowIso8601Utc();
    event.action = record.action;
    event.actor = UserContext::current().actorName();
    event.actorUserId = UserContext::current().account().id;
    event.caseId = record.caseId;
    event.caseNumber = record.caseNumber;
    event.evidenceId = record.evidenceId;
    event.evidenceNumber = record.evidenceNumber;
    event.description =
        record.description.empty() ? std::string(toDisplayString(record.action)) : record.description;
    event.detailsJson = record.details.isObject() ? record.details.dump() : JsonValue::object().dump();
    event.outcome = record.outcome;
    event.appVersion = kApplicationVersion;
    event.host = hostName();

    auto stored = repository_->append(std::move(event));
    if (!stored) {
        logError(kComponent, "Unable to write audit record",
                 JsonValue::object()
                     .set("action", toString(record.action))
                     .set("detail", stored.error().toString()));
    }
    return stored;
}

Result<std::vector<AuditEvent>> AuditService::list(const AuditQuery& query) {
    return repository_->list(query);
}

Result<std::int64_t> AuditService::count(const AuditQuery& query) { return repository_->count(query); }

}  // namespace trace
