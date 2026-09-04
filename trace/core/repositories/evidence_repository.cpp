#include "core/repositories/evidence_repository.h"

#include <utility>

#include "core/common/string_utils.h"

namespace trace {
namespace {

constexpr const char* kColumns =
    "id, case_id, evidence_number, original_filename, stored_filename, source_path, "
    "storage_relpath, media_type, mime_type, file_size, sha256, source_modified_at, "
    "ingested_at, modified_at, ingested_by, integrity_status, last_verified_at, "
    "media_status, description";

Evidence readEvidence(const Statement& stmt) {
    Evidence value;
    value.id = stmt.columnText(0);
    value.caseId = stmt.columnText(1);
    value.evidenceNumber = stmt.columnText(2);
    value.originalFilename = stmt.columnText(3);
    value.storedFilename = stmt.columnText(4);
    value.sourcePath = stmt.columnText(5);
    value.storageRelPath = stmt.columnText(6);
    value.mediaType = mediaTypeFromString(stmt.columnText(7));
    value.mimeType = stmt.columnOptionalText(8);
    value.fileSize = stmt.columnInt64(9);
    value.sha256 = stmt.columnText(10);
    value.sourceModifiedAt = stmt.columnOptionalText(11);
    value.ingestedAt = stmt.columnText(12);
    value.modifiedAt = stmt.columnText(13);
    value.ingestedBy = stmt.columnText(14);
    value.integrityStatus = integrityStatusFromString(stmt.columnText(15));
    value.lastVerifiedAt = stmt.columnOptionalText(16);
    value.mediaStatus = mediaStatusFromString(stmt.columnText(17));
    value.description = stmt.columnText(18);
    return value;
}

}  // namespace

EvidenceRepository::EvidenceRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Status EvidenceRepository::insert(const Evidence& value) {
    auto prepared = database_->prepare(
        std::string("INSERT INTO evidence (") + kColumns +
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, value.id)
        .bind(2, value.caseId)
        .bind(3, value.evidenceNumber)
        .bind(4, value.originalFilename)
        .bind(5, value.storedFilename)
        .bind(6, value.sourcePath)
        .bind(7, value.storageRelPath)
        .bind(8, std::string(toString(value.mediaType)))
        .bindOptional(9, value.mimeType)
        .bind(10, value.fileSize)
        .bind(11, value.sha256)
        .bindOptional(12, value.sourceModifiedAt)
        .bind(13, value.ingestedAt)
        .bind(14, value.modifiedAt)
        .bind(15, value.ingestedBy)
        .bind(16, std::string(toString(value.integrityStatus)))
        .bindOptional(17, value.lastVerifiedAt)
        .bind(18, std::string(toString(value.mediaStatus)))
        .bind(19, value.description);
    return stmt.run();
}

Status EvidenceRepository::update(const Evidence& value) {
    auto prepared = database_->prepare(
        "UPDATE evidence SET evidence_number = ?, media_type = ?, mime_type = ?, "
        "modified_at = ?, integrity_status = ?, last_verified_at = ?, media_status = ?, "
        "description = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, value.evidenceNumber)
        .bind(2, std::string(toString(value.mediaType)))
        .bindOptional(3, value.mimeType)
        .bind(4, value.modifiedAt)
        .bind(5, std::string(toString(value.integrityStatus)))
        .bindOptional(6, value.lastVerifiedAt)
        .bind(7, std::string(toString(value.mediaStatus)))
        .bind(8, value.description)
        .bind(9, value.id);
    if (auto status = stmt.run(); !status) return status;
    if (database_->changes() == 0) {
        return Status::failure(ErrorCode::NotFound, "Evidence not found: " + value.id);
    }
    return Status::success();
}

Status EvidenceRepository::remove(const std::string& evidenceId) {
    auto prepared = database_->prepare("DELETE FROM evidence WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);
    return stmt.run();
}

Status EvidenceRepository::updateIntegrityStatus(const std::string& evidenceId,
                                                 IntegrityStatus status,
                                                 const std::string& verifiedAt) {
    auto prepared = database_->prepare(
        "UPDATE evidence SET integrity_status = ?, last_verified_at = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, std::string(toString(status)))
        .bind(2, verifiedAt)
        .bind(3, evidenceId);
    return stmt.run();
}

Status EvidenceRepository::updateMediaStatus(const std::string& evidenceId, MediaStatus status) {
    auto prepared = database_->prepare("UPDATE evidence SET media_status = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, std::string(toString(status))).bind(2, evidenceId);
    return stmt.run();
}

Result<std::optional<Evidence>> EvidenceRepository::findById(const std::string& evidenceId) {
    using ResultType = Result<std::optional<Evidence>>;
    auto prepared =
        database_->prepare(std::string("SELECT ") + kColumns + " FROM evidence WHERE id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readEvidence(stmt));
}

Result<std::vector<Evidence>> EvidenceRepository::listForCase(const std::string& caseId) {
    using ResultType = Result<std::vector<Evidence>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kColumns +
                                       " FROM evidence WHERE case_id = ? ORDER BY evidence_number;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);

    std::vector<Evidence> results;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        results.push_back(readEvidence(stmt));
    }
    return ResultType::success(std::move(results));
}

Result<std::vector<Evidence>> EvidenceRepository::findByHash(const std::string& sha256,
                                                             const std::string& caseId) {
    using ResultType = Result<std::vector<Evidence>>;
    std::string sql = std::string("SELECT ") + kColumns + " FROM evidence WHERE sha256 = ?";
    if (!caseId.empty()) sql += " AND case_id = ?";
    sql += ";";

    auto prepared = database_->prepare(sql);
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, sha256);
    if (!caseId.empty()) stmt.bind(2, caseId);

    std::vector<Evidence> results;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        results.push_back(readEvidence(stmt));
    }
    return ResultType::success(std::move(results));
}

Result<std::int64_t> EvidenceRepository::countForCase(const std::string& caseId) {
    auto prepared = database_->prepare("SELECT COUNT(*) FROM evidence WHERE case_id = ?;");
    if (!prepared) return Result<std::int64_t>(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);
    auto stepped = stmt.step();
    if (!stepped) return Result<std::int64_t>(stepped.error());
    if (!stepped.value()) return Result<std::int64_t>::success(0);
    return Result<std::int64_t>::success(stmt.columnInt64(0));
}

Result<std::string> EvidenceRepository::nextEvidenceNumber(const std::string& caseId) {
    auto prepared = database_->prepare(
        "SELECT COALESCE(MAX(CAST(SUBSTR(evidence_number, 5) AS INTEGER)), 0) FROM evidence "
        "WHERE case_id = ? AND evidence_number GLOB 'EVD-[0-9]*';");
    if (!prepared) return Result<std::string>(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);
    auto stepped = stmt.step();
    if (!stepped) return Result<std::string>(stepped.error());
    const std::int64_t highest = stepped.value() ? stmt.columnInt64(0) : 0;
    return Result<std::string>::success(formatSequenceNumber("EVD-", highest + 1, 6));
}

}  // namespace trace
