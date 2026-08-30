#include "core/services/integrity_service.h"

#include <utility>

#include "core/common/logging.h"
#include "core/common/time_utils.h"

namespace trace {
namespace {
constexpr const char* kComponent = "integrity";
}

IntegrityService::IntegrityService(std::shared_ptr<Database> database, StorageLayout layout,
                                   std::shared_ptr<AuditService> audit,
                                   std::shared_ptr<WorkspaceKeys> keys)
    : evidence_(std::make_shared<EvidenceRepository>(std::move(database))),
      layout_(std::move(layout)),
      audit_(std::move(audit)),
      keys_(std::move(keys)) {}

Result<IntegrityCheck> IntegrityService::verify(const Evidence& evidence,
                                                const std::string& caseNumber,
                                                const ProgressCallback& progress) {
    using ResultType = Result<IntegrityCheck>;

    IntegrityCheck check;
    check.storedSha256 = evidence.sha256;
    check.checkedAt = nowIso8601Utc();

    const auto path = layout_.resolve(evidence.storageRelPath);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        check.fileMissing = true;
        evidence_->updateIntegrityStatus(evidence.id, IntegrityStatus::Error, check.checkedAt);

        AuditRecord record;
        record.action = AuditAction::IntegrityFailed;
        record.caseId = evidence.caseId;
        record.caseNumber = caseNumber;
        record.evidenceId = evidence.id;
        record.evidenceNumber = evidence.evidenceNumber;
        record.outcome = "failure";
        record.description =
            "Integrity check could not run: the managed original is missing from storage";
        record.details = JsonValue::object()
                             .set("storage_relpath", evidence.storageRelPath)
                             .set("stored_sha256", evidence.sha256)
                             .set("reason", "file_missing");
        audit_->record(record);

        logError(kComponent, "Managed original missing",
                 JsonValue::object()
                     .set("evidence", evidence.evidenceNumber)
                     .set("path", path.string()));
        return ResultType::failure(ErrorCode::NotFound,
                                   "The managed original for " + evidence.evidenceNumber +
                                       " is missing from storage",
                                   path.string());
    }

    // For encrypted evidence this decrypts as it hashes, so the number compared
    // against the stored digest is the digest of the recording, not of the
    // container that happens to hold it today.
    CaseKeyHandle caseKey;
    if (keys_ != nullptr) {
        auto handle = caseKeyFor(*keys_, evidence.caseId);
        if (!handle) return ResultType(handle.error());
        caseKey = handle.take();
    }
    auto hashed = hashStoredEvidence(path, caseKey.get(), progress);
    if (!hashed) {
        if (hashed.error().code() != ErrorCode::Cancelled) {
            evidence_->updateIntegrityStatus(evidence.id, IntegrityStatus::Error, check.checkedAt);
            AuditRecord record;
            record.action = AuditAction::IntegrityFailed;
            record.caseId = evidence.caseId;
            record.caseNumber = caseNumber;
            record.evidenceId = evidence.id;
            record.evidenceNumber = evidence.evidenceNumber;
            record.outcome = "failure";
            record.description = "Integrity check could not complete: " + hashed.error().message();
            record.details = JsonValue::object()
                                 .set("error_code", toString(hashed.error().code()))
                                 .setIfNotEmpty("detail", hashed.error().detail());
            audit_->record(record);
        }
        return ResultType(hashed.error());
    }

    check.computedSha256 = hashed.take();
    check.verified = (check.computedSha256 == check.storedSha256);
    if (const auto size = std::filesystem::file_size(path, ec); !ec) {
        check.bytesRead = static_cast<std::int64_t>(size);
    }

    const IntegrityStatus status =
        check.verified ? IntegrityStatus::Verified : IntegrityStatus::Failed;
    if (auto updated = evidence_->updateIntegrityStatus(evidence.id, status, check.checkedAt);
        !updated) {
        return ResultType(updated.error());
    }

    AuditRecord record;
    record.action = check.verified ? AuditAction::IntegrityVerified : AuditAction::IntegrityFailed;
    record.caseId = evidence.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = evidence.id;
    record.evidenceNumber = evidence.evidenceNumber;
    record.outcome = check.verified ? "success" : "failure";
    record.description = check.verified
                             ? "Integrity verified for " + evidence.evidenceNumber +
                                   ": SHA-256 matches the digest recorded at ingestion"
                             : "INTEGRITY FAILURE for " + evidence.evidenceNumber +
                                   ": SHA-256 does not match the digest recorded at ingestion";
    record.details = JsonValue::object()
                         .set("algorithm", "SHA-256")
                         .set("stored_sha256", check.storedSha256)
                         .set("computed_sha256", check.computedSha256)
                         .set("match", check.verified)
                         .set("bytes_read", check.bytesRead)
                         .set("stored_digest_replaced", false);
    if (auto audited = audit_->record(record); !audited) return ResultType(audited.error());

    if (check.verified) {
        logInfo(kComponent, "Integrity verified",
                JsonValue::object().set("evidence", evidence.evidenceNumber));
    } else {
        logError(kComponent, "Integrity verification failed",
                 JsonValue::object()
                     .set("evidence", evidence.evidenceNumber)
                     .set("stored_sha256", check.storedSha256)
                     .set("computed_sha256", check.computedSha256));
    }
    return ResultType::success(std::move(check));
}

}  // namespace trace
