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
#include "core/security/workspace_keys.h"
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
    /// `keys` may be null, which is what an unencrypted workspace looks like.
    /// When it is present and the workspace is encrypted, every asset registered
    /// here is written into a container before the row is inserted — and when
    /// the workspace is locked, registration fails rather than leaving a
    /// thumbnail of the evidence in the clear beside an encrypted original.
    DerivedAssetService(std::shared_ptr<Database> database, StorageLayout layout,
                        std::shared_ptr<AuditService> audit,
                        std::shared_ptr<WorkspaceKeys> keys = nullptr);

    /// Files a derived artefact with its provenance.
    ///
    /// ## What the recorded digest and size describe
    ///
    /// Both are of the **plaintext** — the image, the envelope, the clip — not
    /// of the container it ends up stored in. That matches how evidence records
    /// its own digest, and for the same reason: the number in the row is what
    /// identifies the content, and it has to stay that number whatever TRACE
    /// does to store it. A report citing the SHA-256 of an exported frame is
    /// citing the frame, and an examiner handed that frame can check it.
    Result<DerivedAsset> registerAsset(const DerivedAssetRegistration& registration);

    /// The key that opens a case's derived assets, or an empty handle when the
    /// workspace is not encrypted. Readers go through here so there is one
    /// answer rather than one per caller.
    Result<CaseKeyHandle> caseKey(const std::string& caseId) const;

    /// Records an operation that produced no derived asset.
    ///
    /// Live capture is the case this exists for: the file it produced is
    /// *original* evidence, not a derivation of anything, so there is no asset
    /// row to write — but the operation that made it is exactly the provenance a
    /// captured recording needs, because for a capture the software, the machine
    /// clock and the camera are the only account of where the material came
    /// from. Routed through here rather than through the repository so
    /// provenance keeps one write path.
    Result<ProcessingOperation> recordOperation(const std::string& caseId,
                                                const std::string& evidenceId,
                                                const std::string& operationType,
                                                const JsonValue& parameters,
                                                const JsonValue& libraryVersions,
                                                const std::string& notes = {},
                                                const std::string& startedAt = {},
                                                const std::string& completedAt = {});

    Result<std::vector<DerivedAsset>> listForEvidence(const std::string& evidenceId);
    Result<std::vector<DerivedAsset>> listForCase(const std::string& caseId);
    Result<std::vector<ProcessingOperation>> operationsForEvidence(const std::string& evidenceId);
    Result<std::int64_t> countForEvidence(const std::string& evidenceId);

private:
    std::shared_ptr<Database> database_;
    std::shared_ptr<ProvenanceRepository> repository_;
    StorageLayout layout_;
    std::shared_ptr<AuditService> audit_;
    std::shared_ptr<WorkspaceKeys> keys_;
};

}  // namespace trace
