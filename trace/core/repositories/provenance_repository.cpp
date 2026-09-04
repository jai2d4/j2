#include "core/repositories/provenance_repository.h"

#include <utility>

namespace trace {
namespace {

constexpr const char* kOperationColumns =
    "id, case_id, evidence_id, operation_type, parameters_json, software_name, "
    "software_version, library_versions, model_name, model_version, source_start_us, "
    "source_end_us, source_frame_number, started_at, completed_at, status, performed_by, "
    "error_message, notes";

constexpr const char* kAssetColumns =
    "id, case_id, evidence_id, operation_id, parent_asset_id, asset_type, storage_relpath, "
    "filename, media_type, file_size, sha256, source_start_us, source_end_us, "
    "source_frame_number, created_at, created_by, verification_state, verified_by, "
    "verified_at, notes";

ProcessingOperation readOperation(const Statement& stmt) {
    ProcessingOperation operation;
    operation.id = stmt.columnText(0);
    operation.caseId = stmt.columnText(1);
    operation.evidenceId = stmt.columnText(2);
    operation.operationType = stmt.columnText(3);
    operation.parametersJson = stmt.columnText(4);
    operation.softwareName = stmt.columnText(5);
    operation.softwareVersion = stmt.columnText(6);
    operation.libraryVersionsJson = stmt.columnText(7);
    operation.modelName = stmt.columnOptionalText(8);
    operation.modelVersion = stmt.columnOptionalText(9);
    operation.sourceStartUs = stmt.columnOptionalInt64(10);
    operation.sourceEndUs = stmt.columnOptionalInt64(11);
    operation.sourceFrameNumber = stmt.columnOptionalInt64(12);
    operation.startedAt = stmt.columnText(13);
    operation.completedAt = stmt.columnOptionalText(14);
    operation.status = stmt.columnText(15);
    operation.performedBy = stmt.columnText(16);
    operation.errorMessage = stmt.columnOptionalText(17);
    operation.notes = stmt.columnText(18);
    return operation;
}

DerivedAsset readAsset(const Statement& stmt) {
    DerivedAsset asset;
    asset.id = stmt.columnText(0);
    asset.caseId = stmt.columnText(1);
    asset.evidenceId = stmt.columnText(2);
    asset.operationId = stmt.columnOptionalText(3);
    asset.parentAssetId = stmt.columnOptionalText(4);
    asset.type = derivedAssetTypeFromString(stmt.columnText(5));
    asset.storageRelPath = stmt.columnText(6);
    asset.filename = stmt.columnText(7);
    asset.mediaType = stmt.columnOptionalText(8);
    asset.fileSize = stmt.columnOptionalInt64(9);
    asset.sha256 = stmt.columnOptionalText(10);
    asset.sourceStartUs = stmt.columnOptionalInt64(11);
    asset.sourceEndUs = stmt.columnOptionalInt64(12);
    asset.sourceFrameNumber = stmt.columnOptionalInt64(13);
    asset.createdAt = stmt.columnText(14);
    asset.createdBy = stmt.columnText(15);
    asset.verificationState = verificationStateFromString(stmt.columnText(16));
    asset.verifiedBy = stmt.columnOptionalText(17);
    asset.verifiedAt = stmt.columnOptionalText(18);
    asset.notes = stmt.columnText(19);
    return asset;
}

}  // namespace

ProvenanceRepository::ProvenanceRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Status ProvenanceRepository::insertOperation(const ProcessingOperation& operation) {
    auto prepared = database_->prepare(
        std::string("INSERT INTO processing_operations (") + kOperationColumns +
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, operation.id)
        .bind(2, operation.caseId)
        .bind(3, operation.evidenceId)
        .bind(4, operation.operationType)
        .bind(5, operation.parametersJson)
        .bind(6, operation.softwareName)
        .bind(7, operation.softwareVersion)
        .bind(8, operation.libraryVersionsJson)
        .bindOptional(9, operation.modelName)
        .bindOptional(10, operation.modelVersion)
        .bindOptional(11, operation.sourceStartUs)
        .bindOptional(12, operation.sourceEndUs)
        .bindOptional(13, operation.sourceFrameNumber)
        .bind(14, operation.startedAt)
        .bindOptional(15, operation.completedAt)
        .bind(16, operation.status)
        .bind(17, operation.performedBy)
        .bindOptional(18, operation.errorMessage)
        .bind(19, operation.notes);
    return stmt.run();
}

Status ProvenanceRepository::completeOperation(const std::string& operationId,
                                               const std::string& completedAt,
                                               const std::string& status,
                                               const std::optional<std::string>& errorMessage) {
    auto prepared = database_->prepare(
        "UPDATE processing_operations SET completed_at = ?, status = ?, error_message = ? "
        "WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, completedAt).bind(2, status).bindOptional(3, errorMessage).bind(4, operationId);
    return stmt.run();
}

Result<std::optional<ProcessingOperation>> ProvenanceRepository::findOperation(
    const std::string& operationId) {
    using ResultType = Result<std::optional<ProcessingOperation>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kOperationColumns +
                                       " FROM processing_operations WHERE id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, operationId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readOperation(stmt));
}

Result<std::vector<ProcessingOperation>> ProvenanceRepository::listOperationsForEvidence(
    const std::string& evidenceId) {
    using ResultType = Result<std::vector<ProcessingOperation>>;
    auto prepared = database_->prepare(
        std::string("SELECT ") + kOperationColumns +
        " FROM processing_operations WHERE evidence_id = ? ORDER BY started_at DESC;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);

    std::vector<ProcessingOperation> operations;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        operations.push_back(readOperation(stmt));
    }
    return ResultType::success(std::move(operations));
}

Status ProvenanceRepository::insertAsset(const DerivedAsset& asset) {
    auto prepared = database_->prepare(
        std::string("INSERT INTO derived_assets (") + kAssetColumns +
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, asset.id)
        .bind(2, asset.caseId)
        .bind(3, asset.evidenceId)
        .bindOptional(4, asset.operationId)
        .bindOptional(5, asset.parentAssetId)
        .bind(6, std::string(toString(asset.type)))
        .bind(7, asset.storageRelPath)
        .bind(8, asset.filename)
        .bindOptional(9, asset.mediaType)
        .bindOptional(10, asset.fileSize)
        .bindOptional(11, asset.sha256)
        .bindOptional(12, asset.sourceStartUs)
        .bindOptional(13, asset.sourceEndUs)
        .bindOptional(14, asset.sourceFrameNumber)
        .bind(15, asset.createdAt)
        .bind(16, asset.createdBy)
        .bind(17, std::string(toString(asset.verificationState)))
        .bindOptional(18, asset.verifiedBy)
        .bindOptional(19, asset.verifiedAt)
        .bind(20, asset.notes);
    return stmt.run();
}

Status ProvenanceRepository::removeAsset(const std::string& assetId) {
    auto prepared = database_->prepare("DELETE FROM derived_assets WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, assetId);
    return stmt.run();
}

Result<std::optional<DerivedAsset>> ProvenanceRepository::findAsset(const std::string& assetId) {
    using ResultType = Result<std::optional<DerivedAsset>>;
    auto prepared =
        database_->prepare(std::string("SELECT ") + kAssetColumns + " FROM derived_assets WHERE id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, assetId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readAsset(stmt));
}

Result<std::vector<DerivedAsset>> ProvenanceRepository::listAssetsForEvidence(
    const std::string& evidenceId) {
    using ResultType = Result<std::vector<DerivedAsset>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kAssetColumns +
                                       " FROM derived_assets WHERE evidence_id = ? ORDER BY created_at DESC;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);

    std::vector<DerivedAsset> assets;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        assets.push_back(readAsset(stmt));
    }
    return ResultType::success(std::move(assets));
}

Result<std::vector<DerivedAsset>> ProvenanceRepository::listAssetsForCase(const std::string& caseId) {
    using ResultType = Result<std::vector<DerivedAsset>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kAssetColumns +
                                       " FROM derived_assets WHERE case_id = ? ORDER BY created_at DESC;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);

    std::vector<DerivedAsset> assets;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        assets.push_back(readAsset(stmt));
    }
    return ResultType::success(std::move(assets));
}

Result<std::int64_t> ProvenanceRepository::countAssetsForEvidence(const std::string& evidenceId) {
    auto prepared = database_->prepare("SELECT COUNT(*) FROM derived_assets WHERE evidence_id = ?;");
    if (!prepared) return Result<std::int64_t>(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);
    auto stepped = stmt.step();
    if (!stepped) return Result<std::int64_t>(stepped.error());
    return Result<std::int64_t>::success(stepped.value() ? stmt.columnInt64(0) : 0);
}

}  // namespace trace
