#pragma once

#include <cstdint>
#include <vector>

#include "core/models/detection.h"

namespace trace {

/// How a source frame was fitted into a model's fixed input canvas, and how to
/// undo it.
///
/// Getting this wrong is the classic detection bug: boxes drift, or sit in the
/// padding. The transform is kept as data and tested independently of any model
/// so the mapping can be verified without running inference.
struct LetterboxTransform {
    double scale = 1.0;      ///< uniform source -> canvas scale factor
    int offsetX = 0;         ///< where the scaled image starts inside the canvas
    int offsetY = 0;
    int scaledWidth = 0;
    int scaledHeight = 0;
    int canvasWidth = 0;
    int canvasHeight = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;

    /// Converts a box in canvas pixels back to normalised source coordinates.
    /// Padding is removed and the result is clamped into the frame, so a box
    /// that overhangs the image edge is trimmed rather than reported outside it.
    NormalizedBox canvasBoxToSource(double x, double y, double width, double height) const;

    /// Converts a centre-form box in canvas pixels (cx, cy, w, h), which is what
    /// most detection heads emit.
    NormalizedBox canvasCenterBoxToSource(double centerX, double centerY, double width,
                                          double height) const;
};

/// Computes the fit. `centred` places the image in the middle of the canvas
/// (padding on both sides); otherwise it sits at the top-left, which is what the
/// YOLOX reference preprocessing does.
LetterboxTransform computeLetterbox(int sourceWidth, int sourceHeight, int canvasWidth,
                                    int canvasHeight, bool centred = false);

/// Resizes packed RGB24 into a padded canvas using bilinear interpolation.
///
/// `output` is filled as packed float in NCHW order — the layout ONNX detection
/// models expect. `channelOrderBgr` swaps R and B for models trained on BGR
/// input, and `scaleTo01` divides by 255 for models that expect normalised
/// input. Both are model properties, so they are parameters rather than
/// assumptions.
void letterboxToTensor(const std::uint8_t* rgb, int sourceWidth, int sourceHeight,
                       const LetterboxTransform& transform, bool channelOrderBgr, bool scaleTo01,
                       std::uint8_t padValue, std::vector<float>& output);

}  // namespace trace
