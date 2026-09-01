#include "core/services/derived_asset_service.h"

#include <utility>

#include "core/common/logging.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/security/file_hasher.h"
#include "core/security/user_context.h"
#include "trace/trace_version.h"

namespace trace {

DerivedAssetService::DerivedAssetService(std::shared_ptr<Database> database, StorageLayout layout,
                                         std::shared_ptr<AuditService> audit)
    : database_(std::move(database)),
      repository_(std::make_shared<ProvenanceRepository>(database_)),
      layout_(std::move(layout)),
      audit_(std::move(audit)) {}

Result<DerivedAsset> DerivedAssetService::registerAsset(
    const DerivedAssetRegistration& registration) {
    using ResultType = Result<DerivedAsset>;

    std::error_code ec;
    if (!std::filesystem::exists(registration.file, ec)) {
        return ResultType::failure(ErrorCode::NotFound,
                                   "Derived file was not written: " + registration.file.string());
    }

    auto hashed = hashFile(registration.file);
    if (!hashed) return ResultType(hashed.error());

    const std::string now = nowIso8601Utc();
    const std::string actor = UserContext::current().actorName();

    ProcessingOperation operation;
    operation.id = generateUuid();
    operation.caseId = registration.caseId;
    operation.evidenceId = registration.evidenceId;
    operation.operationType = registration.operationType;
    operation.parametersJson = registration.parameters.dump();
    operation.softwareName = kApplicationName;
    operation.softwareVersion = kApplicationVersion;
    operation.libraryVersionsJson = registration.libraryVersions.dump();
    operation.modelName = registration.modelName;
    operation.modelVersion = registration.modelVersion;
    operation.sourceStartUs = registration.sourceStartUs;
    operation.sourceEndUs = registration.sourceEndUs;
    operation.sourceFrameNumber = registration.sourceFrameNumber;
    operation.startedAt = now;
    operation.completedAt = now;
    operation.status = "completed";
    operation.performedBy = actor;

    DerivedAsset asset;
    asset.id = generateUuid();
    asset.caseId = registration.caseId;
    asset.evidenceId = registration.evidenceId;
    asset.operationId = operation.id;
    asset.parentAssetId = registration.parentAssetId;
    asset.type = registration.type;
    asset.storageRelPath = layout_.relativeTo(registration.file);
    asset.filename = registration.file.filename().string();
    asset.mediaType = registration.mediaType;
    if (const auto size = std::filesystem::file_size(registration.file, ec); !ec) {
        asset.fileSize = static_cast<std::int64_t>(size);
    }
    asset.sha256 = hashed.take();
    asset.sourceStartUs = registration.sourceStartUs;
    asset.sourceEndUs = registration.sourceEndUs;
    asset.sourceFrameNumber = registration.sourceFrameNumber;
    asset.createdAt = now;
    asset.createdBy = actor;
    // Machine output is never self-certified; an analyst marks it verified.
    asset.verificationState = VerificationState::Unverified;
    asset.notes = registration.notes;

    {
        Transaction transaction(*database_);
        if (auto status = transaction.begin(); !status) return ResultType(status.error());
        if (auto status = repository_->insertOperation(operation); !status) {
            transaction.rollback();
            return ResultType(status.error());
        }
        if (auto status = repository_->insertAsset(asset); !status) {
            transaction.rollback();
            return ResultType(status.error());
        }
        if (auto status = transaction.commit(); !status) return ResultType(status.error());
    }

    AuditRecord record;
    record.action = registration.auditAction;
    record.caseId = registration.caseId;
    record.caseNumber = registration.caseNumber;
    record.evidenceId = registration.evidenceId;
    record.evidenceNumber = registration.evidenceNumber;
    record.description =
        registration.auditDescription.empty()
            ? std::string(toDisplayString(registration.type)) + " created from " +
                  registration.evidenceNumber
            : registration.auditDescription;
    record.details = JsonValue::object()
                         .set("asset_id", asset.id)
                         .set("operation_id", operation.id)
                         .set("operation_type", operation.operationType)
                         .set("asset_type", toString(asset.type))
                         .set("filename", asset.filename)
                         .set("storage_relpath", asset.storageRelPath)
                         .set("sha256", asset.sha256.value_or(""))
                         .set("software", operation.softwareName + " " + operation.softwareVersion)
                         .set("parameters", registration.parameters)
                         .set("library_versions", registration.libraryVersions);
    if (asset.sourceStartUs) {
        record.details.set("source_timestamp_us", *asset.sourceStartUs)
            .set("source_timecode", formatTimecode(*asset.sourceStartUs));
    }
    if (asset.sourceFrameNumber) record.details.set("source_frame_number", *asset.sourceFrameNumber);
    if (auto audited = audit_->record(record); !audited) return ResultType(audited.error());

    logInfo("provenance", "Derived asset registered",
            JsonValue::object()
                .set("asset_type", toString(asset.type))
                .set("evidence", registration.evidenceNumber)
                .set("operation", operation.operationType));

    return ResultType::success(std::move(asset));
}

Result<ProcessingOperation> DerivedAssetService::recordOperation(
    const std::string& caseId, const std::string& evidenceId, const std::string& operationType,
    const JsonValue& parameters, const JsonValue& libraryVersions, const std::string& notes,
    const std::string& startedAt, const std::string& completedAt) {
    using ResultType = Result<ProcessingOperation>;

    const std::string now = nowIso8601Utc();

    ProcessingOperation operation;
    operation.id = generateUuid();
    operation.caseId = caseId;
    operation.evidenceId = evidenceId;
    operation.operationType = operationType;
    operation.parametersJson = parameters.dump();
    operation.softwareName = kApplicationName;
    operation.softwareVersion = kApplicationVersion;
    operation.libraryVersionsJson = libraryVersions.dump();
    // The caller's own times where it has them. A capture's operation ran for as
    // long as the recording did, and collapsing that to the instant the row was
    // written would lose the one span an examiner is most likely to ask about.
    operation.startedAt = startedAt.empty() ? now : startedAt;
    operation.completedAt = completedAt.empty() ? now : completedAt;
    operation.status = "completed";
    operation.performedBy = UserContext::current().actorName();
    operation.notes = notes;

    if (auto status = repository_->insertOperation(operation); !status) {
        return ResultType(status.error());
    }
    return ResultType::success(std::move(operation));
}

Result<std::vector<DerivedAsset>> DerivedAssetService::listForEvidence(
    const std::string& evidenceId) {
    return repository_->listAssetsForEvidence(evidenceId);
}

Result<std::vector<DerivedAsset>> DerivedAssetService::listForCase(const std::string& caseId) {
    return repository_->listAssetsForCase(caseId);
}

Result<std::vector<ProcessingOperation>> DerivedAssetService::operationsForEvidence(
    const std::string& evidenceId) {
    return repository_->listOperationsForEvidence(evidenceId);
}

Result<std::int64_t> DerivedAssetService::countForEvidence(const std::string& evidenceId) {
    return repository_->countAssetsForEvidence(evidenceId);
}

}  // namespace trace
