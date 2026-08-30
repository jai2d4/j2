#include "core/services/evidence_service.h"

#include <algorithm>
#include <utility>

#include "core/common/logging.h"
#include "core/common/string_utils.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/database/database.h"
#include "core/repositories/case_repository.h"
#include "core/security/user_context.h"
#include "core/storage/file_operations.h"

namespace trace {
namespace {

constexpr const char* kComponent = "ingest";

/// Relative weights of the import stages, used to turn per-stage byte progress
/// into a single honest overall percentage.
constexpr double kCopyWeight = 0.55;
constexpr double kVerifyWeight = 0.30;
constexpr double kMetadataWeight = 0.10;
constexpr double kRecordWeight = 0.05;

struct ExtensionMapping {
    const char* extension;
    MediaType type;
    const char* mime;
};

constexpr ExtensionMapping kExtensionMap[] = {
    {".mp4", MediaType::Video, "video/mp4"},
    {".m4v", MediaType::Video, "video/x-m4v"},
    {".mov", MediaType::Video, "video/quicktime"},
    {".avi", MediaType::Video, "video/x-msvideo"},
    {".mkv", MediaType::Video, "video/x-matroska"},
    {".webm", MediaType::Video, "video/webm"},
    {".mpg", MediaType::Video, "video/mpeg"},
    {".mpeg", MediaType::Video, "video/mpeg"},
    {".ts", MediaType::Video, "video/mp2t"},
    {".m2ts", MediaType::Video, "video/mp2t"},
    {".wmv", MediaType::Video, "video/x-ms-wmv"},
    {".flv", MediaType::Video, "video/x-flv"},
    {".3gp", MediaType::Video, "video/3gpp"},
    {".mxf", MediaType::Video, "application/mxf"},
    {".dav", MediaType::Video, ""},          // common CCTV container
    {".wav", MediaType::Audio, "audio/wav"},
    {".mp3", MediaType::Audio, "audio/mpeg"},
    {".m4a", MediaType::Audio, "audio/mp4"},
    {".aac", MediaType::Audio, "audio/aac"},
    {".flac", MediaType::Audio, "audio/flac"},
    {".ogg", MediaType::Audio, "audio/ogg"},
    {".opus", MediaType::Audio, "audio/opus"},
    {".amr", MediaType::Audio, "audio/amr"},
    {".jpg", MediaType::Image, "image/jpeg"},
    {".jpeg", MediaType::Image, "image/jpeg"},
    {".png", MediaType::Image, "image/png"},
    {".bmp", MediaType::Image, "image/bmp"},
    {".gif", MediaType::Image, "image/gif"},
    {".tif", MediaType::Image, "image/tiff"},
    {".tiff", MediaType::Image, "image/tiff"},
    {".heic", MediaType::Image, "image/heic"},
    {".webp", MediaType::Image, "image/webp"},
    {".pdf", MediaType::Document, "application/pdf"},
    {".txt", MediaType::Document, "text/plain"},
    {".csv", MediaType::Document, "text/csv"},
    {".docx", MediaType::Document,
     "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
};

bool reportProgress(const IngestProgressCallback& callback, IngestStage stage,
                    const std::string& message, double fraction, std::int64_t done = 0,
                    std::int64_t total = 0) {
    if (!callback) return true;
    IngestProgress progress;
    progress.stage = stage;
    progress.message = message;
    progress.bytesProcessed = done;
    progress.bytesTotal = total;
    progress.overallFraction = std::clamp(fraction, 0.0, 1.0);
    return callback(progress);
}

/// Classifies media from what the container actually reported, falling back to
/// the extension only when extraction produced nothing.
MediaType classify(const std::optional<MediaMetadata>& metadata,
                   const std::filesystem::path& source) {
    if (metadata && metadata->extractionStatus != MetadataExtractionStatus::Failed) {
        if (metadata->videoStreamCount > 0) {
            // A container with a single still image stream is an image, not video.
            const bool stillImage = metadata->audioStreamCount == 0 &&
                                    metadata->containerFormat &&
                                    (containsCaseInsensitive(*metadata->containerFormat, "image2") ||
                                     containsCaseInsensitive(*metadata->containerFormat, "_pipe") ||
                                     containsCaseInsensitive(*metadata->containerFormat, "jpeg") ||
                                     containsCaseInsensitive(*metadata->containerFormat, "png"));
            return stillImage ? MediaType::Image : MediaType::Video;
        }
        if (metadata->audioStreamCount > 0) return MediaType::Audio;
    }
    return mediaTypeFromExtension(source);
}

}  // namespace

const char* toDisplayString(IngestStage stage) {
    switch (stage) {
        case IngestStage::Validating:        return "Validating source file";
        case IngestStage::PreparingStorage:  return "Preparing managed storage";
        case IngestStage::CopyingOriginal:   return "Copying original and calculating SHA-256";
        case IngestStage::VerifyingCopy:     return "Verifying the copy against the source";
        case IngestStage::ExtractingMetadata:return "Extracting metadata";
        case IngestStage::CreatingRecord:    return "Creating evidence record";
        case IngestStage::Complete:          return "Complete";
        case IngestStage::Failed:            return "Failed";
    }
    return "Working";
}

MediaType mediaTypeFromExtension(const std::filesystem::path& path) {
    const std::string extension = toLower(path.extension().string());
    for (const auto& entry : kExtensionMap) {
        if (extension == entry.extension) return entry.type;
    }
    return MediaType::Unknown;
}

std::string mimeTypeFromExtension(const std::filesystem::path& path) {
    const std::string extension = toLower(path.extension().string());
    for (const auto& entry : kExtensionMap) {
        if (extension == entry.extension) return entry.mime;
    }
    return {};
}

EvidenceService::EvidenceService(std::shared_ptr<Database> database, StorageLayout layout,
                                 std::shared_ptr<AuditService> audit,
                                 std::shared_ptr<IMetadataExtractor> extractor,
                                 std::shared_ptr<WorkspaceKeys> keys)
    : database_(std::move(database)),
      evidence_(std::make_shared<EvidenceRepository>(database_)),
      metadata_(std::make_shared<MetadataRepository>(database_)),
      layout_(std::move(layout)),
      audit_(std::move(audit)),
      extractor_(std::move(extractor)),
      keys_(std::move(keys)) {}

Result<CaseKeyHandle> EvidenceService::caseKey(const std::string& caseId) const {
    if (keys_ == nullptr) return Result<CaseKeyHandle>::success(CaseKeyHandle());
    return caseKeyFor(*keys_, caseId);
}

Result<IngestOutcome> EvidenceService::ingest(const IngestRequest& request,
                                              const IngestProgressCallback& progress) {
    using ResultType = Result<IngestOutcome>;

    const auto fail = [&](const Error& error, const std::string& caseNumber) {
        reportProgress(progress, IngestStage::Failed, error.message(), 1.0);
        AuditRecord record;
        record.action = AuditAction::EvidenceImportFailed;
        record.caseId = request.caseId;
        if (!caseNumber.empty()) record.caseNumber = caseNumber;
        record.outcome = "failure";
        record.description = "Evidence import failed: " + error.message();
        record.details = JsonValue::object()
                             .set("source_path", request.sourcePath.string())
                             .set("error_code", toString(error.code()))
                             .setIfNotEmpty("detail", error.detail())
                             .set("source_modified", false);
        audit_->record(record);
        logError(kComponent, "Import failed",
                 JsonValue::object()
                     .set("source", request.sourcePath.string())
                     .set("error", error.toString()));
    };

    // ---------------------------------------------------------- 1. validate
    if (!UserContext::current().can(Permission::ImportEvidence)) {
        const Error error(ErrorCode::PermissionDenied,
                          "You do not have permission to import evidence");
        fail(error, {});
        return ResultType(error);
    }
    if (!reportProgress(progress, IngestStage::Validating, "Validating source file", 0.0)) {
        return ResultType::failure(ErrorCode::Cancelled, "Import cancelled by operator");
    }

    CaseRepository caseRepository(database_);
    auto caseFound = caseRepository.findById(request.caseId);
    if (!caseFound) return ResultType(caseFound.error());
    if (!caseFound.value().has_value()) {
        const Error error(ErrorCode::NotFound, "The case for this import no longer exists");
        fail(error, {});
        return ResultType(error);
    }
    const Case owningCase = *caseFound.take();

    std::error_code ec;
    if (!std::filesystem::exists(request.sourcePath, ec) ||
        std::filesystem::is_directory(request.sourcePath, ec)) {
        const Error error(ErrorCode::NotFound,
                          "Source file not found: " + request.sourcePath.string());
        fail(error, owningCase.caseNumber);
        return ResultType(error);
    }
    const auto sourceSize = std::filesystem::file_size(request.sourcePath, ec);
    if (ec) {
        const Error error(ErrorCode::IoError, "Unable to read the source file", ec.message());
        fail(error, owningCase.caseNumber);
        return ResultType(error);
    }

    IngestOutcome outcome;
    Evidence evidence;
    evidence.id = generateUuid();
    evidence.caseId = owningCase.id;
    evidence.originalFilename = request.sourcePath.filename().string();
    evidence.sourcePath = std::filesystem::absolute(request.sourcePath, ec).string();
    if (ec) evidence.sourcePath = request.sourcePath.string();
    evidence.fileSize = static_cast<std::int64_t>(sourceSize);
    evidence.sourceModifiedAt = fileModifiedTimeIso8601(request.sourcePath);
    evidence.ingestedAt = nowIso8601Utc();
    evidence.modifiedAt = evidence.ingestedAt;
    evidence.ingestedBy = UserContext::current().actorName();
    evidence.description = request.description;
    evidence.mediaType = mediaTypeFromExtension(request.sourcePath);
    if (const std::string mime = mimeTypeFromExtension(request.sourcePath); !mime.empty()) {
        evidence.mimeType = mime;
    }

    auto numbered = evidence_->nextEvidenceNumber(owningCase.id);
    if (!numbered) {
        fail(numbered.error(), owningCase.caseNumber);
        return ResultType(numbered.error());
    }
    evidence.evidenceNumber = numbered.take();

    {
        AuditRecord record;
        record.action = AuditAction::EvidenceImportStarted;
        record.caseId = owningCase.id;
        record.caseNumber = owningCase.caseNumber;
        record.evidenceId = evidence.id;
        record.evidenceNumber = evidence.evidenceNumber;
        record.description = "Import started for " + evidence.originalFilename;
        record.details = JsonValue::object()
                             .set("source_path", evidence.sourcePath)
                             .set("file_size", evidence.fileSize);
        audit_->record(record);
    }

    // --------------------------------------------------- 2. prepare storage
    if (!reportProgress(progress, IngestStage::PreparingStorage, "Preparing managed storage", 0.02)) {
        return ResultType::failure(ErrorCode::Cancelled, "Import cancelled by operator");
    }
    if (auto status = layout_.ensureCaseDirectories(owningCase.id); !status) {
        fail(status.error(), owningCase.caseNumber);
        return ResultType(status.error());
    }

    evidence.storedFilename =
        StorageLayout::managedOriginalFilename(evidence.id, evidence.originalFilename);
    const auto destination = layout_.originalsDirectory(owningCase.id) / evidence.storedFilename;
    evidence.storageRelPath = layout_.relativeTo(destination);

    // ------------------------------------- 3./4. copy, hash, verify the copy
    bool cancelled = false;
    auto copyProgress = [&](const HashProgress& state) {
        // The copy pass hashes the source; the verify pass re-reads the copy.
        // Both are reported against their own weight so the bar never stalls.
        const double fraction =
            state.totalBytes > 0
                ? static_cast<double>(state.bytesProcessed) / static_cast<double>(state.totalBytes)
                : 0.0;
        const bool keepGoing =
            reportProgress(progress, IngestStage::CopyingOriginal,
                           "Copying original and calculating SHA-256",
                           0.02 + fraction * kCopyWeight, state.bytesProcessed, state.totalBytes);
        if (!keepGoing) cancelled = true;
        return keepGoing;
    };

    // Resolved before the copy starts: a locked workspace has to stop ingestion
    // here, not after the evidence has already been written somewhere.
    auto caseKeyResult = caseKey(request.caseId);
    if (!caseKeyResult) return ResultType(caseKeyResult.error());
    const CaseKeyHandle key = caseKeyResult.take();

    auto copied = copyIntoManagedStorage(request.sourcePath, destination, copyProgress, key.get());
    if (!copied) {
        if (cancelled || copied.error().code() == ErrorCode::Cancelled) {
            removeManagedFile(destination);
            const Error error(ErrorCode::Cancelled, "Import cancelled by operator");
            fail(error, owningCase.caseNumber);
            return ResultType(error);
        }
        fail(copied.error(), owningCase.caseNumber);
        return ResultType(copied.error());
    }
    const CopyOutcome copyOutcome = copied.take();
    evidence.sha256 = copyOutcome.destinationSha256;

    if (!reportProgress(progress, IngestStage::VerifyingCopy,
                        "Copy verified against source digest", 0.02 + kCopyWeight + kVerifyWeight)) {
        removeManagedFile(destination);
        const Error error(ErrorCode::Cancelled, "Import cancelled by operator");
        fail(error, owningCase.caseNumber);
        return ResultType(error);
    }

    // The managed original is byte-identical and now read-only.
    setReadOnly(destination);
    evidence.integrityStatus = IntegrityStatus::Verified;
    evidence.lastVerifiedAt = nowIso8601Utc();

    {
        AuditRecord record;
        record.action = AuditAction::EvidenceHashed;
        record.caseId = owningCase.id;
        record.caseNumber = owningCase.caseNumber;
        record.evidenceId = evidence.id;
        record.evidenceNumber = evidence.evidenceNumber;
        record.description = "SHA-256 calculated for " + evidence.evidenceNumber;
        record.details = JsonValue::object()
                             .set("algorithm", "SHA-256")
                             .set("sha256", evidence.sha256)
                             .set("source_sha256", copyOutcome.sourceSha256)
                             .set("bytes", copyOutcome.bytesCopied)
                             .set("copy_matches_source", true);
        audit_->record(record);
    }

    // Duplicate detection is advisory: the same footage may legitimately arrive
    // from two custodians, and the operator decides what that means.
    auto duplicates = evidence_->findByHash(evidence.sha256, owningCase.id);
    if (duplicates && !duplicates.value().empty()) {
        outcome.duplicateOfExisting = true;
        outcome.duplicateEvidenceNumber = duplicates.value().front().evidenceNumber;
        if (!request.allowDuplicate) {
            removeManagedFile(destination);
            const Error error(ErrorCode::AlreadyExists,
                              "This file is already in the case as " +
                                  outcome.duplicateEvidenceNumber);
            fail(error, owningCase.caseNumber);
            return ResultType(error);
        }
    }

    // ------------------------------------------------- 5. extract metadata
    std::optional<MediaMetadata> metadata;
    if (extractor_ != nullptr) {
        if (!reportProgress(progress, IngestStage::ExtractingMetadata, "Extracting metadata",
                            0.02 + kCopyWeight + kVerifyWeight)) {
            removeManagedFile(destination);
            const Error error(ErrorCode::Cancelled, "Import cancelled by operator");
            fail(error, owningCase.caseNumber);
            return ResultType(error);
        }
        auto extracted = extractor_->extract(destination, evidence.id, key.get());
        if (extracted) {
            metadata = extracted.take();
            outcome.metadataExtracted =
                metadata->extractionStatus != MetadataExtractionStatus::Failed;
            evidence.mediaStatus = outcome.metadataExtracted ? MediaStatus::Decodable
                                                            : MediaStatus::MetadataFailed;
        } else {
            // Unreadable metadata is a media limitation, never an integrity
            // finding: the bytes are intact and stay intact.
            outcome.metadataError = extracted.error().toString();
            evidence.mediaStatus = extracted.error().code() == ErrorCode::Unsupported
                                       ? MediaStatus::Unsupported
                                       : MediaStatus::MetadataFailed;
            metadata = MediaMetadata{};
            metadata->evidenceId = evidence.id;
            metadata->extractionStatus = MetadataExtractionStatus::Failed;
            metadata->extractionError = extracted.error().toString();
            metadata->extractor = extractor_->name();
            metadata->extractedAt = nowIso8601Utc();
            logWarn(kComponent, "Metadata extraction failed",
                    JsonValue::object()
                        .set("evidence", evidence.evidenceNumber)
                        .set("detail", outcome.metadataError));
        }
    }
    evidence.mediaType = classify(metadata, request.sourcePath);
    if (evidence.mediaType == MediaType::Unknown && metadata &&
        metadata->extractionStatus == MetadataExtractionStatus::Failed) {
        evidence.mediaStatus = MediaStatus::NotMedia;
    }

    // ------------------------------------------------ 6. persist the record
    if (!reportProgress(progress, IngestStage::CreatingRecord, "Creating evidence record",
                        0.02 + kCopyWeight + kVerifyWeight + kMetadataWeight)) {
        removeManagedFile(destination);
        const Error error(ErrorCode::Cancelled, "Import cancelled by operator");
        fail(error, owningCase.caseNumber);
        return ResultType(error);
    }

    {
        Transaction transaction(*database_);
        if (auto status = transaction.begin(); !status) {
            removeManagedFile(destination);
            fail(status.error(), owningCase.caseNumber);
            return ResultType(status.error());
        }
        if (auto status = evidence_->insert(evidence); !status) {
            transaction.rollback();
            removeManagedFile(destination);
            fail(status.error(), owningCase.caseNumber);
            return ResultType(status.error());
        }
        if (metadata) {
            if (auto status = metadata_->save(*metadata); !status) {
                transaction.rollback();
                removeManagedFile(destination);
                fail(status.error(), owningCase.caseNumber);
                return ResultType(status.error());
            }
        }
        if (auto status = transaction.commit(); !status) {
            removeManagedFile(destination);
            fail(status.error(), owningCase.caseNumber);
            return ResultType(status.error());
        }
    }

    // ---------------------------------------------------------- 7. audit it
    {
        AuditRecord record;
        record.action = AuditAction::EvidenceImported;
        record.caseId = owningCase.id;
        record.caseNumber = owningCase.caseNumber;
        record.evidenceId = evidence.id;
        record.evidenceNumber = evidence.evidenceNumber;
        record.description =
            "Evidence " + evidence.evidenceNumber + " imported: " + evidence.originalFilename;
        record.details = JsonValue::object()
                             .set("original_filename", evidence.originalFilename)
                             .set("stored_filename", evidence.storedFilename)
                             .set("source_path", evidence.sourcePath)
                             .set("storage_relpath", evidence.storageRelPath)
                             .set("file_size", evidence.fileSize)
                             .set("sha256", evidence.sha256)
                             .set("media_type", toString(evidence.mediaType))
                             .set("media_status", toString(evidence.mediaStatus))
                             .set("original_source_modified", false);
        if (outcome.duplicateOfExisting) {
            record.details.set("duplicate_of", outcome.duplicateEvidenceNumber);
            record.outcome = "warning";
        }
        if (auto audited = audit_->record(record); !audited) return ResultType(audited.error());
    }
    if (metadata) {
        AuditRecord record;
        record.action = outcome.metadataExtracted ? AuditAction::EvidenceMetadataExtracted
                                                  : AuditAction::EvidenceMetadataFailed;
        record.caseId = owningCase.id;
        record.caseNumber = owningCase.caseNumber;
        record.evidenceId = evidence.id;
        record.evidenceNumber = evidence.evidenceNumber;
        record.outcome = outcome.metadataExtracted ? "success" : "warning";
        record.description = outcome.metadataExtracted
                                 ? "Technical metadata extracted for " + evidence.evidenceNumber
                                 : "Metadata could not be extracted for " + evidence.evidenceNumber;
        record.details = JsonValue::object()
                             .set("extractor", metadata->extractor)
                             .set("status", toString(metadata->extractionStatus));
        if (metadata->containerFormat) record.details.set("container", *metadata->containerFormat);
        if (metadata->videoCodec) record.details.set("video_codec", *metadata->videoCodec);
        if (metadata->durationUs) record.details.set("duration_us", *metadata->durationUs);
        if (metadata->extractionError) record.details.set("error", *metadata->extractionError);
        audit_->record(record);
    }

    reportProgress(progress, IngestStage::Complete, "Complete", 1.0, evidence.fileSize,
                   evidence.fileSize);
    logInfo(kComponent, "Evidence imported",
            JsonValue::object()
                .set("evidence", evidence.evidenceNumber)
                .set("case", owningCase.caseNumber)
                .set("sha256", evidence.sha256)
                .set("bytes", evidence.fileSize));

    outcome.evidence = std::move(evidence);
    outcome.metadata = std::move(metadata);
    return ResultType::success(std::move(outcome));
}

Result<std::vector<Evidence>> EvidenceService::listForCase(const std::string& caseId) {
    return evidence_->listForCase(caseId);
}

Result<std::optional<Evidence>> EvidenceService::findById(const std::string& evidenceId) {
    return evidence_->findById(evidenceId);
}

Result<std::optional<MediaMetadata>> EvidenceService::metadataFor(const std::string& evidenceId) {
    return metadata_->findByEvidenceId(evidenceId);
}

Result<std::int64_t> EvidenceService::countForCase(const std::string& caseId) {
    return evidence_->countForCase(caseId);
}

std::filesystem::path EvidenceService::absolutePath(const Evidence& evidence) const {
    return layout_.resolve(evidence.storageRelPath);
}

Status EvidenceService::recordViewed(const Evidence& evidence, const std::string& caseNumber) {
    AuditRecord record;
    record.action = AuditAction::EvidenceViewed;
    record.caseId = evidence.caseId;
    record.caseNumber = caseNumber;
    record.evidenceId = evidence.id;
    record.evidenceNumber = evidence.evidenceNumber;
    record.description = "Evidence " + evidence.evidenceNumber + " opened in the viewer";
    auto audited = audit_->record(record);
    if (!audited) return Status(audited.error());
    return Status::success();
}

Result<MediaMetadata> EvidenceService::reextractMetadata(const Evidence& evidence) {
    using ResultType = Result<MediaMetadata>;
    if (extractor_ == nullptr) {
        return ResultType::failure(ErrorCode::Unsupported,
                                   "No metadata extractor is available in this build");
    }
    auto reextractKey = caseKey(evidence.caseId);
    if (!reextractKey) return Result<MediaMetadata>(reextractKey.error());
    const CaseKeyHandle reKey = reextractKey.take();
    auto extracted = extractor_->extract(absolutePath(evidence), evidence.id, reKey.get());
    if (!extracted) {
        evidence_->updateMediaStatus(evidence.id, MediaStatus::MetadataFailed);
        return ResultType(extracted.error());
    }
    MediaMetadata metadata = extracted.take();
    if (auto status = metadata_->save(metadata); !status) return ResultType(status.error());
    evidence_->updateMediaStatus(evidence.id,
                                 metadata.extractionStatus == MetadataExtractionStatus::Failed
                                     ? MediaStatus::MetadataFailed
                                     : MediaStatus::Decodable);
    return ResultType::success(std::move(metadata));
}

}  // namespace trace
