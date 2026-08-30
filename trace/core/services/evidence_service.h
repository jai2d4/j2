#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/models/case.h"
#include "core/models/evidence.h"
#include "core/models/media_metadata.h"
#include "core/repositories/evidence_repository.h"
#include "core/repositories/metadata_repository.h"
#include "core/security/workspace_keys.h"
#include "core/services/audit_service.h"
#include "core/services/metadata_extractor.h"
#include "core/storage/storage_layout.h"

namespace trace {

/// Ordered stages of an import, surfaced to the progress UI.
enum class IngestStage {
    Validating,
    PreparingStorage,
    CopyingOriginal,
    VerifyingCopy,
    ExtractingMetadata,
    CreatingRecord,
    Complete,
    Failed,
};

const char* toDisplayString(IngestStage stage);

struct IngestProgress {
    IngestStage stage = IngestStage::Validating;
    std::string message;
    std::int64_t bytesProcessed = 0;
    std::int64_t bytesTotal = 0;
    double overallFraction = 0.0;  ///< 0..1 across the whole import
};

/// Return false to cancel. Cancellation rolls the import back completely.
using IngestProgressCallback = std::function<bool(const IngestProgress&)>;

struct IngestRequest {
    std::string caseId;
    std::filesystem::path sourcePath;
    std::string description;
    /// When false, an import whose digest already exists in the case is
    /// rejected instead of creating a second record for the same bytes.
    bool allowDuplicate = true;
};

struct IngestOutcome {
    Evidence evidence;
    std::optional<MediaMetadata> metadata;
    bool metadataExtracted = false;
    std::string metadataError;
    bool duplicateOfExisting = false;
    std::string duplicateEvidenceNumber;
};

/// Import and retrieval of evidence.
///
/// The import sequence is: validate, prepare storage, copy the original into
/// managed storage while hashing it, re-read the copy to prove it matches,
/// extract metadata (best effort), then write the database records inside one
/// transaction. Any fatal failure removes the managed copy and leaves no
/// evidence row behind; the operator's source file is never touched.
class EvidenceService {
public:
    /// `keys` may be null, which is what an unencrypted workspace looks like.
    /// When it is present and the workspace is locked, ingestion fails rather
    /// than writing evidence in the clear into a workspace the operator has been
    /// told is encrypted.
    EvidenceService(std::shared_ptr<Database> database, StorageLayout layout,
                    std::shared_ptr<AuditService> audit,
                    std::shared_ptr<IMetadataExtractor> extractor = nullptr,
                    std::shared_ptr<WorkspaceKeys> keys = nullptr);

    Result<IngestOutcome> ingest(const IngestRequest& request,
                                 const IngestProgressCallback& progress = {});

    Result<std::vector<Evidence>> listForCase(const std::string& caseId);
    Result<std::optional<Evidence>> findById(const std::string& evidenceId);
    Result<std::optional<MediaMetadata>> metadataFor(const std::string& evidenceId);
    Result<std::int64_t> countForCase(const std::string& caseId);

    /// Absolute path of the managed original.
    std::filesystem::path absolutePath(const Evidence& evidence) const;

    /// The key that opens a case's evidence, or an empty handle when the
    /// workspace is not encrypted. Everything that decodes, hashes or exports
    /// evidence goes through here, so there is one answer rather than one per
    /// caller.
    Result<CaseKeyHandle> caseKey(const std::string& caseId) const;

    /// Records that an analyst opened the item in the viewer.
    Status recordViewed(const Evidence& evidence, const std::string& caseNumber);

    /// Re-runs metadata extraction for an item whose first attempt failed.
    Result<MediaMetadata> reextractMetadata(const Evidence& evidence);

    const StorageLayout& layout() const { return layout_; }

private:
    std::shared_ptr<Database> database_;
    std::shared_ptr<EvidenceRepository> evidence_;
    std::shared_ptr<MetadataRepository> metadata_;
    StorageLayout layout_;
    std::shared_ptr<AuditService> audit_;
    std::shared_ptr<IMetadataExtractor> extractor_;
    std::shared_ptr<WorkspaceKeys> keys_;

};

/// Best-effort media classification from a filename, used before the container
/// has been read and as a fallback when extraction fails.
MediaType mediaTypeFromExtension(const std::filesystem::path& path);
/// Conventional MIME type for a filename, or empty when unknown. TRACE does not
/// invent one.
std::string mimeTypeFromExtension(const std::filesystem::path& path);

}  // namespace trace
