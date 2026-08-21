#include "core/repositories/audit_repository.h"

#include <utility>

#include "core/common/string_utils.h"

namespace trace {
namespace {

constexpr const char* kColumns =
    "id, sequence, occurred_at, action, actor, case_id, case_number, evidence_id, "
    "evidence_number, description, details_json, outcome, app_version, host, actor_user_id";

AuditEvent readEvent(const Statement& stmt) {
    AuditEvent event;
    event.id = stmt.columnText(0);
    event.sequence = stmt.columnInt64(1);
    event.occurredAt = stmt.columnText(2);
    event.action = auditActionFromString(stmt.columnText(3));
    event.actor = stmt.columnText(4);
    event.caseId = stmt.columnOptionalText(5);
    event.caseNumber = stmt.columnOptionalText(6);
    event.evidenceId = stmt.columnOptionalText(7);
    event.evidenceNumber = stmt.columnOptionalText(8);
    event.description = stmt.columnText(9);
    event.detailsJson = stmt.columnText(10);
    event.outcome = stmt.columnText(11);
    event.appVersion = stmt.columnText(12);
    event.host = stmt.columnText(13);
    event.actorUserId = stmt.columnOptionalText(14);
    return event;
}

/// Builds the shared WHERE clause. Parameter indices are fixed so both list()
/// and count() bind identically.
std::string buildWhere(const AuditQuery& query) {
    std::vector<std::string> conditions;
    if (query.caseId) conditions.emplace_back("case_id = ?1");
    if (query.evidenceId) conditions.emplace_back("evidence_id = ?2");
    if (query.action) conditions.emplace_back("action = ?3");
    if (!trim(query.text).empty()) {
        conditions.emplace_back("(description LIKE ?4 OR actor LIKE ?4 OR action LIKE ?4)");
    }
    if (conditions.empty()) return {};
    return " WHERE " + join(conditions, " AND ");
}

void bindQuery(Statement& stmt, const AuditQuery& query) {
    if (query.caseId) stmt.bind(1, *query.caseId);
    if (query.evidenceId) stmt.bind(2, *query.evidenceId);
    if (query.action) stmt.bind(3, std::string(toString(*query.action)));
    const std::string text = trim(query.text);
    if (!text.empty()) stmt.bind(4, "%" + text + "%");
}

}  // namespace

AuditRepository::AuditRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Result<AuditEvent> AuditRepository::append(AuditEvent event) {
    using ResultType = Result<AuditEvent>;

    // The sequence read and the insert must not interleave with another writer.
    auto guard = database_->lockGuard();

    auto highest = database_->queryInt64("SELECT COALESCE(MAX(sequence), 0) FROM audit_events;");
    if (!highest) return ResultType(highest.error());
    event.sequence = highest.value() + 1;

    auto prepared = database_->prepare(std::string("INSERT INTO audit_events (") + kColumns +
                                       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return ResultType(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, event.id)
        .bind(2, event.sequence)
        .bind(3, event.occurredAt)
        .bind(4, std::string(toString(event.action)))
        .bind(5, event.actor)
        .bindOptional(6, event.caseId)
        .bindOptional(7, event.caseNumber)
        .bindOptional(8, event.evidenceId)
        .bindOptional(9, event.evidenceNumber)
        .bind(10, event.description)
        .bind(11, event.detailsJson)
        .bind(12, event.outcome)
        .bind(13, event.appVersion)
        .bind(14, event.host)
        .bindOptional(15, event.actorUserId);
    if (auto status = stmt.run(); !status) return ResultType(status.error());
    return ResultType::success(std::move(event));
}

Result<std::vector<AuditEvent>> AuditRepository::list(const AuditQuery& query) {
    using ResultType = Result<std::vector<AuditEvent>>;

    std::string sql = std::string("SELECT ") + kColumns + " FROM audit_events" + buildWhere(query);
    sql += query.newestFirst ? " ORDER BY sequence DESC" : " ORDER BY sequence ASC";
    if (query.limit > 0) {
        sql += " LIMIT " + std::to_string(query.limit);
        if (query.offset > 0) sql += " OFFSET " + std::to_string(query.offset);
    }
    sql += ";";

    auto prepared = database_->prepare(sql);
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    bindQuery(stmt, query);

    std::vector<AuditEvent> events;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        events.push_back(readEvent(stmt));
    }
    return ResultType::success(std::move(events));
}

Result<std::int64_t> AuditRepository::count(const AuditQuery& query) {
    const std::string sql = "SELECT COUNT(*) FROM audit_events" + buildWhere(query) + ";";
    auto prepared = database_->prepare(sql);
    if (!prepared) return Result<std::int64_t>(prepared.error());
    Statement stmt = prepared.take();
    bindQuery(stmt, query);
    auto stepped = stmt.step();
    if (!stepped) return Result<std::int64_t>(stepped.error());
    return Result<std::int64_t>::success(stepped.value() ? stmt.columnInt64(0) : 0);
}

Result<std::int64_t> AuditRepository::countForAction(AuditAction action) {
    AuditQuery query;
    query.action = action;
    return count(query);
}

}  // namespace trace
