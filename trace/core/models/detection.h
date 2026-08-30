#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace trace {

/// Coarse grouping used by the timeline lanes and the detection filters.
///
/// A model reports fine-grained classes ("car", "bus", "truck"); TRACE groups
/// them so an analyst can ask for "vehicles" without knowing the class list of
/// whichever model produced the results.
enum class DetectionClassGroup { Person, Vehicle, Object };

const char* toString(DetectionClassGroup group);
const char* toDisplayString(DetectionClassGroup group);
DetectionClassGroup detectionClassGroupFromString(const std::string& text,
                                                  DetectionClassGroup fallback = DetectionClassGroup::Object);

/// Analyst review state of a machine-produced detection.
///
/// Everything starts Unreviewed. TRACE never promotes its own output, and a
/// rejected detection is kept — it is part of the analytical history, not a
/// mistake to be erased.
enum class DetectionVerification { Unreviewed, Confirmed, Rejected, Uncertain };

const char* toString(DetectionVerification state);
const char* toDisplayString(DetectionVerification state);
DetectionVerification detectionVerificationFromString(const std::string& text,
                                                      DetectionVerification fallback = DetectionVerification::Unreviewed);

/// An axis-aligned box in normalised source-frame coordinates.
///
/// Origin is the top-left of the frame; x/y/width/height are fractions of the
/// source width and height in the range 0..1. Normalised geometry is the
/// authoritative form because it stays correct when the same evidence is drawn
/// at another size — in a resized window, full screen, or a report.
struct NormalizedBox {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    double right() const { return x + width; }
    double bottom() const { return y + height; }
    double area() const { return width > 0 && height > 0 ? width * height : 0.0; }
    bool valid() const {
        return width > 0.0 && height > 0.0 && x >= 0.0 && y >= 0.0 && right() <= 1.0001 &&
               bottom() <= 1.0001;
    }
    /// Clamps the box into the unit square, discarding area that fell outside
    /// the frame rather than reporting coordinates that cannot exist.
    NormalizedBox clampedToFrame() const;
};

/// One object reported by a model at one moment of one evidence item.
///
/// `timestampUs` is the presentation timestamp of the analysed frame, taken from
/// the media rather than computed from a frame index, so it stays correct on
/// variable-frame-rate recordings.
struct Detection {
    std::string id;
    std::string analysisRunId;
    std::string caseId;
    std::string evidenceId;

    std::int64_t timestampUs = 0;
    std::optional<std::int64_t> frameNumber;

    int classId = 0;
    std::string classLabel;
    DetectionClassGroup classGroup = DetectionClassGroup::Object;
    double confidence = 0.0;

    NormalizedBox box;
    std::optional<std::int64_t> pixelX;
    std::optional<std::int64_t> pixelY;
    std::optional<std::int64_t> pixelWidth;
    std::optional<std::int64_t> pixelHeight;

    DetectionVerification verification = DetectionVerification::Unreviewed;
    std::optional<std::string> verifiedBy;
    std::optional<std::string> verifiedAt;
    std::string analystNote;

    std::string createdAt;
};

}  // namespace trace
