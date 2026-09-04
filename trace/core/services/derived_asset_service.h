#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/json.h"
#include "core/common/result.h"
#include "core/models/derived_asset.h"
#include "core/repositories/provenance_repository.h"
#include "core/services/audit_service.h"
#include "core/storage/storage_layout.h"

namespace trace {

/// Everything needed to file a derived asset with full provenance. The file
/// must already exist inside managed storage; this service records what
/// produced it and hashes what was written.
struct DerivedAssetRegistration {
    std::string caseId;
    std::string caseNumber;
    std::string evidenceId;
    std::string evidenceNumber;
    DerivedAssetType type = DerivedAssetType::Other;
    std::filesystem::path file;

    std::string operationType;             ///< e.g. "frame_extraction"
    JsonValue parameters = JsonValue::object();
    JsonValue libraryVersions = JsonValue::object();
    std::optional<std::string> modelName;  ///< future analysis providers
    std::optional<std::string> modelVersion;

    std::optional<std::int64_t> sourceStartUs;
    std::optional<std::int64_t> sourceEndUs;
    std::optional<std::int64_t> sourceFrameNumber;

    std::optional<std::string> mediaType;
    std::optional<std::string> parentAssetId;
    std::string notes;
    AuditAction auditAction = AuditAction::DerivedAssetCreated;
    std::string auditDescription;
};

/// Registers derived assets and the operations that produced them.
///
/// This is the single write path for provenance: every future module that turns
/// evidence into something else — a frame, a clip, a transcript, a detection
/// set — files it here and inherits the same chain.
class DerivedAssetService {
public:
    DerivedAssetService(std::shared_ptr<Database> database, StorageLayout layout,
                        std::shared_ptr<AuditService> audit);

    Result<DerivedAsset> registerAsset(const DerivedAssetRegistration& registration);

    Result<std::vector<DerivedAsset>> listForEvidence(const std::string& evidenceId);
    Result<std::vector<DerivedAsset>> listForCase(const std::string& caseId);
    Result<std::vector<ProcessingOperation>> operationsForEvidence(const std::string& evidenceId);
    Result<std::int64_t> countForEvidence(const std::string& evidenceId);

private:
    std::shared_ptr<Database> database_;
    std::shared_ptr<ProvenanceRepository> repository_;
    StorageLayout layout_;
    std::shared_ptr<AuditService> audit_;
};

}  // namespace trace
