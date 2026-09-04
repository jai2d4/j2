#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace trace {

/// Annotation kinds. Phase 0 creates the first two; the rest are declared now so
/// the persisted vocabulary does not change when drawing tools arrive — a
/// bounding box is the same row with geometry attached.
enum class AnnotationType {
    TimestampNote,  ///< a note pinned to a single moment
    RangeNote,      ///< a note covering [start, end]
    Point,          ///< a marked image coordinate at a moment
    Region,         ///< a bounding box / polygon over a range
    Arrow,
    Measurement,
};

const char* toString(AnnotationType type);
const char* toDisplayString(AnnotationType type);
AnnotationType annotationTypeFromString(const std::string& text,
                                        AnnotationType fallback = AnnotationType::TimestampNote);

/// A timestamped observation made by an analyst.
struct Annotation {
    std::string id;
    std::string caseId;
    std::string evidenceId;
    AnnotationType type = AnnotationType::TimestampNote;
    std::int64_t startUs = 0;
    std::optional<std::int64_t> endUs;      ///< empty for point-in-time notes
    std::optional<std::int64_t> startFrame;
    std::optional<std::int64_t> endFrame;
    std::string text;
    /// Reserved for future geometry (boxes, arrows, measurements). Phase 0
    /// leaves this empty rather than writing a placeholder.
    std::optional<std::string> geometryJson;
    std::optional<std::string> colour;
    std::string createdBy;
    std::string createdAt;
    std::string modifiedAt;
};

}  // namespace trace
