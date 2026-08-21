#include "core/repositories/bookmark_repository.h"

#include <utility>

namespace trace {
namespace {

constexpr const char* kColumns =
    "id, case_id, evidence_id, timestamp_us, frame_number, title, note, colour, created_by, "
    "created_at, modified_at";

Bookmark readBookmark(const Statement& stmt) {
    Bookmark value;
    value.id = stmt.columnText(0);
    value.caseId = stmt.columnText(1);
    value.evidenceId = stmt.columnText(2);
    value.timestampUs = stmt.columnInt64(3);
    value.frameNumber = stmt.columnOptionalInt64(4);
    value.title = stmt.columnText(5);
    value.note = stmt.columnText(6);
    value.colour = stmt.columnOptionalText(7);
    value.createdBy = stmt.columnText(8);
    value.createdAt = stmt.columnText(9);
    value.modifiedAt = stmt.columnText(10);
    return value;
}

}  // namespace

BookmarkRepository::BookmarkRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Status BookmarkRepository::insert(const Bookmark& value) {
    auto prepared = database_->prepare(std::string("INSERT INTO bookmarks (") + kColumns +
                                       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, value.id)
        .bind(2, value.caseId)
        .bind(3, value.evidenceId)
        .bind(4, value.timestampUs)
        .bindOptional(5, value.frameNumber)
        .bind(6, value.title)
        .bind(7, value.note)
        .bindOptional(8, value.colour)
        .bind(9, value.createdBy)
        .bind(10, value.createdAt)
        .bind(11, value.modifiedAt);
    return stmt.run();
}

Status BookmarkRepository::update(const Bookmark& value) {
    auto prepared = database_->prepare(
        "UPDATE bookmarks SET timestamp_us = ?, frame_number = ?, title = ?, note = ?, "
        "colour = ?, modified_at = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, value.timestampUs)
        .bindOptional(2, value.frameNumber)
        .bind(3, value.title)
        .bind(4, value.note)
        .bindOptional(5, value.colour)
        .bind(6, value.modifiedAt)
        .bind(7, value.id);
    if (auto status = stmt.run(); !status) return status;
    if (database_->changes() == 0) {
        return Status::failure(ErrorCode::NotFound, "Bookmark not found: " + value.id);
    }
    return Status::success();
}

Status BookmarkRepository::remove(const std::string& bookmarkId) {
    auto prepared = database_->prepare("DELETE FROM bookmarks WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, bookmarkId);
    return stmt.run();
}

Result<std::optional<Bookmark>> BookmarkRepository::findById(const std::string& bookmarkId) {
    using ResultType = Result<std::optional<Bookmark>>;
    auto prepared =
        database_->prepare(std::string("SELECT ") + kColumns + " FROM bookmarks WHERE id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, bookmarkId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readBookmark(stmt));
}

Result<std::vector<Bookmark>> BookmarkRepository::listForEvidence(const std::string& evidenceId) {
    using ResultType = Result<std::vector<Bookmark>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kColumns +
                                       " FROM bookmarks WHERE evidence_id = ? ORDER BY timestamp_us;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);

    std::vector<Bookmark> results;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        results.push_back(readBookmark(stmt));
    }
    return ResultType::success(std::move(results));
}

Result<std::vector<Bookmark>> BookmarkRepository::listForCase(const std::string& caseId) {
    using ResultType = Result<std::vector<Bookmark>>;
    auto prepared = database_->prepare(
        std::string("SELECT ") + kColumns +
        " FROM bookmarks WHERE case_id = ? ORDER BY evidence_id, timestamp_us;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);

    std::vector<Bookmark> results;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        results.push_back(readBookmark(stmt));
    }
    return ResultType::success(std::move(results));
}

Result<std::int64_t> BookmarkRepository::countForEvidence(const std::string& evidenceId) {
    auto prepared = database_->prepare("SELECT COUNT(*) FROM bookmarks WHERE evidence_id = ?;");
    if (!prepared) return Result<std::int64_t>(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);
    auto stepped = stmt.step();
    if (!stepped) return Result<std::int64_t>(stepped.error());
    return Result<std::int64_t>::success(stepped.value() ? stmt.columnInt64(0) : 0);
}

}  // namespace trace
