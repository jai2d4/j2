#pragma once

#include <vector>

#include "ai/detection/detection_types.h"
#include "ai/detection/preprocessing/letterbox.h"

namespace trace {

/// Intersection over union of two normalised boxes.
double intersectionOverUnion(const NormalizedBox& a, const NormalizedBox& b);

/// Class-aware non-maximum suppression.
///
/// Applied only when the model does not already do it — see
/// DetectionCapabilities::performsOwnNms. Suppression is per class, so a person
/// standing in front of a car does not remove the car.
///
/// Input need not be sorted; the result is ordered by descending confidence and
/// truncated to `maximumKept`.
std::vector<RawDetection> nonMaximumSuppression(std::vector<RawDetection> detections,
                                                double iouThreshold, int maximumKept);

/// Decodes a YOLOX-style raw output tensor.
///
/// Layout is [1, anchors, 5 + classes] with anchors laid out per stride
/// (8, 16, 32), row-major within each stride's grid. Each row holds
/// (dx, dy, dw, dh, objectness, class scores...), where the box is relative to
/// its grid cell: centre = (d + cell) * stride, size = exp(d) * stride, in
/// model-canvas pixels. The score for a class is objectness * class score.
///
/// This decoding is a property of the model family, not of TRACE: another family
/// gets its own decoder behind the same provider interface.
std::vector<RawDetection> decodeYoloxOutput(const float* data, int anchorCount, int valuesPerAnchor,
                                            int canvasWidth, int canvasHeight,
                                            const std::vector<int>& strides,
                                            const std::vector<std::string>& classes,
                                            double confidenceThreshold,
                                            const LetterboxTransform& transform);

}  // namespace trace
