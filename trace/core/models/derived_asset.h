#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace trace {

/// What a derived asset is. Original evidence is never one of these — an
/// original is only ever the ingested file itself.
enum class DerivedAssetType {
    FrameExport,     ///< a single decoded frame written to disk
    Thumbnail,       ///< a generated preview
    WorkingCopy,     ///< analysis-safe transcode of an original
    Clip,            ///< an extracted time range
    AudioExtract,
    AnalysisOutput,  ///< future AI/model output (Phase 1+)
    Report,
    Other,
};

const char* toString(DerivedAssetType type);
const char* toDisplayString(DerivedAssetType type);
DerivedAssetType derivedAssetTypeFromString(const std::string& text,
                                            DerivedAssetType fallback = DerivedAssetType::Other);

/// Whether an analyst has reviewed and accepted the derived result. Machine
/// output stays 'unverified' until a human says otherwise — TRACE never marks
/// its own work as confirmed.
enum class VerificationState { Unverified, HumanVerified, Rejected };

const char* toString(VerificationState state);
const char* toDisplayString(VerificationState state);
VerificationState verificationStateFromString(const std::string& text,
                                              VerificationState fallback = VerificationState::Unverified);

/// The operation that produced one or more derived assets.
///
/// This is the provenance record: what was run, over which source range, with
/// which parameters, by which software/model version, when and by whom.
struct ProcessingOperation {
    std::string id;
    std::string caseId;
    std::string evidenceId;         ///< source evidence
    std::string operationType;      ///< e.g. "frame_extraction", "thumbnail", "analysis"
    std::string parametersJson = "{}";
    std::string softwareName;
    std::string softwareVersion;
    std::string libraryVersionsJson = "{}";  ///< FFmpeg/SQLite/etc versions in effect
    std::optional<std::string> modelName;    ///< populated by future analysis providers
    std::optional<std::string> modelVersion;
    std::optional<std::int64_t> sourceStartUs;
    std::optional<std::int64_t> sourceEndUs;
    std::optional<std::int64_t> sourceFrameNumber;
    std::string startedAt;
    std::optional<std::string> completedAt;
    std::string status = "completed";
    std::string performedBy;
    std::optional<std::string> errorMessage;
    std::string notes;
};

/// A file produced from evidence. Always linked to its source evidence and, in
/// normal operation, to the operation that created it.
struct DerivedAsset {
    std::string id;
    std::string caseId;
    std::string evidenceId;
    std::optional<std::string> operationId;
    std::optional<std::string> parentAssetId;  ///< derivations of derivations
    DerivedAssetType type = DerivedAssetType::Other;
    std::string storageRelPath;
    std::string filename;
    std::optional<std::string> mediaType;
    std::optional<std::int64_t> fileSize;
    std::optional<std::string> sha256;
    std::optional<std::int64_t> sourceStartUs;
    std::optional<std::int64_t> sourceEndUs;
    std::optional<std::int64_t> sourceFrameNumber;
    std::string createdAt;
    std::string createdBy;
    VerificationState verificationState = VerificationState::Unverified;
    std::optional<std::string> verifiedBy;
    std::optional<std::string> verifiedAt;
    std::string notes;
};

}  // namespace trace
