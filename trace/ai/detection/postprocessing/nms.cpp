#include "ai/detection/postprocessing/nms.h"

#include <algorithm>
#include <cmath>

#include "ai/detection/models/class_taxonomy.h"
#include "ai/detection/preprocessing/letterbox.h"

namespace trace {

double intersectionOverUnion(const NormalizedBox& a, const NormalizedBox& b) {
    const double left = std::max(a.x, b.x);
    const double top = std::max(a.y, b.y);
    const double right = std::min(a.right(), b.right());
    const double bottom = std::min(a.bottom(), b.bottom());
    if (right <= left || bottom <= top) return 0.0;

    const double intersection = (right - left) * (bottom - top);
    const double combined = a.area() + b.area() - intersection;
    return combined > 0.0 ? intersection / combined : 0.0;
}

std::vector<RawDetection> nonMaximumSuppression(std::vector<RawDetection> detections,
                                                double iouThreshold, int maximumKept) {
    std::sort(detections.begin(), detections.end(),
              [](const RawDetection& a, const RawDetection& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<RawDetection> kept;
    std::vector<bool> suppressed(detections.size(), false);

    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (suppressed[i]) continue;
        kept.push_back(detections[i]);
        if (maximumKept > 0 && static_cast<int>(kept.size()) >= maximumKept) break;

        for (std::size_t j = i + 1; j < detections.size(); ++j) {
            if (suppressed[j]) continue;
            // Per class: overlapping boxes of different classes are both real.
            if (detections[j].classId != detections[i].classId) continue;
            if (intersectionOverUnion(detections[i].box, detections[j].box) > iouThreshold) {
                suppressed[j] = true;
            }
        }
    }
    return kept;
}

std::vector<RawDetection> decodeYoloxOutput(const float* data, int anchorCount,
                                            int valuesPerAnchor, int canvasWidth, int canvasHeight,
                                            const std::vector<int>& strides,
                                            const std::vector<std::string>& classes,
                                            double confidenceThreshold,
                                            const LetterboxTransform& transform) {
    std::vector<RawDetection> detections;
    if (data == nullptr || anchorCount <= 0 || valuesPerAnchor <= 5) return detections;

    const int classCount = valuesPerAnchor - 5;
    int anchorIndex = 0;

    for (const int stride : strides) {
        if (stride <= 0) continue;
        const int gridWidth = canvasWidth / stride;
        const int gridHeight = canvasHeight / stride;

        for (int row = 0; row < gridHeight; ++row) {
            for (int column = 0; column < gridWidth; ++column, ++anchorIndex) {
                if (anchorIndex >= anchorCount) return detections;
                const float* anchor = data + static_cast<std::size_t>(anchorIndex) * valuesPerAnchor;

                const double objectness = anchor[4];
                // Cheap rejection before scanning the class scores.
                if (objectness < confidenceThreshold * 0.05) continue;

                int bestClass = -1;
                double bestScore = 0.0;
                for (int classIndex = 0; classIndex < classCount; ++classIndex) {
                    const double score = objectness * anchor[5 + classIndex];
                    if (score > bestScore) {
                        bestScore = score;
                        bestClass = classIndex;
                    }
                }
                if (bestClass < 0 || bestScore < confidenceThreshold) continue;

                // Grid-relative centre, exponential size, both in canvas pixels.
                const double strideValue = stride;
                const double centerX = (static_cast<double>(anchor[0]) + column) * strideValue;
                const double centerY = (static_cast<double>(anchor[1]) + row) * strideValue;
                const double width = std::exp(static_cast<double>(anchor[2])) * strideValue;
                const double height = std::exp(static_cast<double>(anchor[3])) * strideValue;

                RawDetection detection;
                detection.classId = bestClass;
                detection.classLabel = labelForClassId(classes, bestClass);
                detection.classGroup = classGroupForLabel(detection.classLabel);
                detection.confidence = std::clamp(bestScore, 0.0, 1.0);
                detection.box = transform.canvasCenterBoxToSource(centerX, centerY, width, height);
                if (detection.box.valid()) detections.push_back(std::move(detection));
            }
        }
    }
    return detections;
}

}  // namespace trace
