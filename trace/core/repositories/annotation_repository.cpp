#include "core/repositories/annotation_repository.h"

#include <utility>

namespace trace {
namespace {

constexpr const char* kColumns =
    "id, case_id, evidence_id, annotation_type, start_us, end_us, start_frame, end_frame, "
    "text, geometry_json, colour, created_by, created_at, modified_at";

Annotation readAnnotation(const Statement& stmt) {
    Annotation value;
    value.id = stmt.columnText(0);
    value.caseId = stmt.columnText(1);
    value.evidenceId = stmt.columnText(2);
    value.type = annotationTypeFromString(stmt.columnText(3));
    value.startUs = stmt.columnInt64(4);
    value.endUs = stmt.columnOptionalInt64(5);
    value.startFrame = stmt.columnOptionalInt64(6);
    value.endFrame = stmt.columnOptionalInt64(7);
    value.text = stmt.columnText(8);
    value.geometryJson = stmt.columnOptionalText(9);
    value.colour = stmt.columnOptionalText(10);
    value.createdBy = stmt.columnText(11);
    value.createdAt = stmt.columnText(12);
    value.modifiedAt = stmt.columnText(13);
    return value;
}

}  // namespace

AnnotationRepository::AnnotationRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Status AnnotationRepository::insert(const Annotation& value) {
    auto prepared = database_->prepare(std::string("INSERT INTO annotations (") + kColumns +
                                       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, value.id)
        .bind(2, value.caseId)
        .bind(3, value.evidenceId)
        .bind(4, std::string(toString(value.type)))
        .bind(5, value.startUs)
        .bindOptional(6, value.endUs)
        .bindOptional(7, value.startFrame)
        .bindOptional(8, value.endFrame)
        .bind(9, value.text)
        .bindOptional(10, value.geometryJson)
        .bindOptional(11, value.colour)
        .bind(12, value.createdBy)
        .bind(13, value.createdAt)
        .bind(14, value.modifiedAt);
    return stmt.run();
}

Status AnnotationRepository::update(const Annotation& value) {
    auto prepared = database_->prepare(
        "UPDATE annotations SET annotation_type = ?, start_us = ?, end_us = ?, start_frame = ?, "
        "end_frame = ?, text = ?, geometry_json = ?, colour = ?, modified_at = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, std::string(toString(value.type)))
        .bind(2, value.startUs)
        .bindOptional(3, value.endUs)
        .bindOptional(4, value.startFrame)
        .bindOptional(5, value.endFrame)
        .bind(6, value.text)
        .bindOptional(7, value.geometryJson)
        .bindOptional(8, value.colour)
        .bind(9, value.modifiedAt)
        .bind(10, value.id);
    if (auto status = stmt.run(); !status) return status;
    if (database_->changes() == 0) {
        return Status::failure(ErrorCode::NotFound, "Annotation not found: " + value.id);
    }
    return Status::success();
}

Status AnnotationRepository::remove(const std::string& annotationId) {
    auto prepared = database_->prepare("DELETE FROM annotations WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, annotationId);
    return stmt.run();
}

Result<std::optional<Annotation>> AnnotationRepository::findById(const std::string& annotationId) {
    using ResultType = Result<std::optional<Annotation>>;
    auto prepared =
        database_->prepare(std::string("SELECT ") + kColumns + " FROM annotations WHERE id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, annotationId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readAnnotation(stmt));
}

Result<std::vector<Annotation>> AnnotationRepository::listForEvidence(const std::string& evidenceId) {
    using ResultType = Result<std::vector<Annotation>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kColumns +
                                       " FROM annotations WHERE evidence_id = ? ORDER BY start_us;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);

    std::vector<Annotation> results;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        results.push_back(readAnnotation(stmt));
    }
    return ResultType::success(std::move(results));
}

Result<std::vector<Annotation>> AnnotationRepository::listForCase(const std::string& caseId) {
    using ResultType = Result<std::vector<Annotation>>;
    auto prepared = database_->prepare(
        std::string("SELECT ") + kColumns +
        " FROM annotations WHERE case_id = ? ORDER BY evidence_id, start_us;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);

    std::vector<Annotation> results;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        results.push_back(readAnnotation(stmt));
    }
    return ResultType::success(std::move(results));
}

Result<std::int64_t> AnnotationRepository::countForEvidence(const std::string& evidenceId) {
    auto prepared = database_->prepare("SELECT COUNT(*) FROM annotations WHERE evidence_id = ?;");
    if (!prepared) return Result<std::int64_t>(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);
    auto stepped = stmt.step();
    if (!stepped) return Result<std::int64_t>(stepped.error());
    return Result<std::int64_t>::success(stepped.value() ? stmt.columnInt64(0) : 0);
}

}  // namespace trace
