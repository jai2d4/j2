#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace trace {

/// Broad class of an evidence file. Determined from the container/streams that
/// FFmpeg actually reports, not from the file extension alone.
enum class MediaType { Unknown, Video, Audio, Image, Document, Other };

const char* toString(MediaType type);
const char* toDisplayString(MediaType type);
MediaType mediaTypeFromString(const std::string& text, MediaType fallback = MediaType::Unknown);

/// Result of the last cryptographic integrity check of the managed original.
/// NotVerified is the honest default: nothing has been re-checked since
/// ingestion.
enum class IntegrityStatus { NotVerified, Verified, Failed, Verifying, Error };

const char* toString(IntegrityStatus status);
const char* toDisplayString(IntegrityStatus status);
IntegrityStatus integrityStatusFromString(const std::string& text,
                                          IntegrityStatus fallback = IntegrityStatus::NotVerified);

/// Whether TRACE can decode the media. Deliberately separate from integrity: a
/// bit-perfect file in an exotic codec is intact but not (yet) decodable, and
/// must never be labelled corrupt.
enum class MediaStatus { Unknown, Decodable, MetadataFailed, Unsupported, NotMedia };

const char* toString(MediaStatus status);
const char* toDisplayString(MediaStatus status);
MediaStatus mediaStatusFromString(const std::string& text,
                                  MediaStatus fallback = MediaStatus::Unknown);

/// An imported evidence item.
///
/// `storageRelPath` points at the managed original, which is read-only after
/// ingestion. `sourcePath` records where it came from, for the chain of
/// custody, and is never written to.
struct Evidence {
    std::string id;                 ///< UUID
    std::string caseId;
    std::string evidenceNumber;     ///< e.g. EVD-000001, unique within the case
    std::string originalFilename;   ///< as supplied by the operator
    std::string storedFilename;     ///< collision-safe managed name
    std::string sourcePath;         ///< original location at ingestion time
    std::string storageRelPath;     ///< relative to the TRACE data directory
    MediaType mediaType = MediaType::Unknown;
    std::optional<std::string> mimeType;
    std::int64_t fileSize = 0;
    std::string sha256;             ///< digest of the managed original
    std::optional<std::string> sourceModifiedAt;
    std::string ingestedAt;
    std::string modifiedAt;
    std::string ingestedBy;
    IntegrityStatus integrityStatus = IntegrityStatus::NotVerified;
    std::optional<std::string> lastVerifiedAt;
    MediaStatus mediaStatus = MediaStatus::Unknown;
    std::string description;
};

}  // namespace trace
