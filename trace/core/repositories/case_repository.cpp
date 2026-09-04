#include "core/repositories/case_repository.h"

#include <utility>

#include "core/common/string_utils.h"

namespace trace {
namespace {

constexpr const char* kColumns =
    "id, case_number, title, description, status, investigator, agency, location, notes, "
    "created_at, modified_at, created_by, storage_relpath";

Case readCase(const Statement& stmt) {
    Case value;
    value.id = stmt.columnText(0);
    value.caseNumber = stmt.columnText(1);
    value.title = stmt.columnText(2);
    value.description = stmt.columnText(3);
    value.status = caseStatusFromString(stmt.columnText(4));
    value.investigator = stmt.columnText(5);
    value.agency = stmt.columnText(6);
    value.location = stmt.columnText(7);
    value.notes = stmt.columnText(8);
    value.createdAt = stmt.columnText(9);
    value.modifiedAt = stmt.columnText(10);
    value.createdBy = stmt.columnText(11);
    value.storageRelPath = stmt.columnText(12);
    return value;
}

}  // namespace

CaseRepository::CaseRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Status CaseRepository::insert(const Case& value) {
    auto prepared = database_->prepare(
        std::string("INSERT INTO cases (") + kColumns +
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, value.id)
        .bind(2, value.caseNumber)
        .bind(3, value.title)
        .bind(4, value.description)
        .bind(5, std::string(toString(value.status)))
        .bind(6, value.investigator)
        .bind(7, value.agency)
        .bind(8, value.location)
        .bind(9, value.notes)
        .bind(10, value.createdAt)
        .bind(11, value.modifiedAt)
        .bind(12, value.createdBy)
        .bind(13, value.storageRelPath);
    return stmt.run();
}

Status CaseRepository::update(const Case& value) {
    auto prepared = database_->prepare(
        "UPDATE cases SET case_number = ?, title = ?, description = ?, status = ?, "
        "investigator = ?, agency = ?, location = ?, notes = ?, modified_at = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, value.caseNumber)
        .bind(2, value.title)
        .bind(3, value.description)
        .bind(4, std::string(toString(value.status)))
        .bind(5, value.investigator)
        .bind(6, value.agency)
        .bind(7, value.location)
        .bind(8, value.notes)
        .bind(9, value.modifiedAt)
        .bind(10, value.id);
    if (auto status = stmt.run(); !status) return status;
    if (database_->changes() == 0) {
        return Status::failure(ErrorCode::NotFound, "Case not found: " + value.id);
    }
    return Status::success();
}

Status CaseRepository::remove(const std::string& caseId) {
    auto prepared = database_->prepare("DELETE FROM cases WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);
    return stmt.run();
}

Result<std::optional<Case>> CaseRepository::findById(const std::string& caseId) {
    using ResultType = Result<std::optional<Case>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kColumns + " FROM cases WHERE id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readCase(stmt));
}

Result<std::optional<Case>> CaseRepository::findByCaseNumber(const std::string& caseNumber) {
    using ResultType = Result<std::optional<Case>>;
    auto prepared =
        database_->prepare(std::string("SELECT ") + kColumns + " FROM cases WHERE case_number = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseNumber);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readCase(stmt));
}

Result<std::vector<Case>> CaseRepository::list(const CaseQuery& query) {
    using ResultType = Result<std::vector<Case>>;

    std::string sql = std::string("SELECT ") + kColumns + " FROM cases";
    std::vector<std::string> conditions;
    const std::string text = trim(query.text);
    if (!text.empty()) {
        conditions.emplace_back(
            "(case_number LIKE ?1 OR title LIKE ?1 OR investigator LIKE ?1 OR agency LIKE ?1 "
            "OR location LIKE ?1 OR description LIKE ?1)");
    }
    if (query.status) conditions.emplace_back("status = ?2");
    if (!conditions.empty()) sql += " WHERE " + join(conditions, " AND ");
    sql += " ORDER BY modified_at DESC";
    if (query.limit > 0) sql += " LIMIT " + std::to_string(query.limit);
    sql += ";";

    auto prepared = database_->prepare(sql);
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    if (!text.empty()) stmt.bind(1, "%" + text + "%");
    if (query.status) stmt.bind(2, std::string(toString(*query.status)));

    std::vector<Case> results;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        results.push_back(readCase(stmt));
    }
    return ResultType::success(std::move(results));
}

Result<std::int64_t> CaseRepository::count() {
    return database_->queryInt64("SELECT COUNT(*) FROM cases;");
}

Result<std::string> CaseRepository::nextCaseNumber() {
    // Only numbers matching the generated CASE-nnnn shape take part in the
    // sequence; an operator is free to type their own agency reference.
    auto value = database_->queryInt64(
        "SELECT COALESCE(MAX(CAST(SUBSTR(case_number, 6) AS INTEGER)), 0) FROM cases "
        "WHERE case_number GLOB 'CASE-[0-9]*';");
    if (!value) return Result<std::string>(value.error());
    return Result<std::string>::success(formatSequenceNumber("CASE-", value.value() + 1, 4));
}

}  // namespace trace
