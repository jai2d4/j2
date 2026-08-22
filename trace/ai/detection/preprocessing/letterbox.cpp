#include "ai/detection/preprocessing/letterbox.h"

#include <algorithm>
#include <cmath>

namespace trace {

LetterboxTransform computeLetterbox(int sourceWidth, int sourceHeight, int canvasWidth,
                                    int canvasHeight, bool centred) {
    LetterboxTransform transform;
    transform.sourceWidth = sourceWidth;
    transform.sourceHeight = sourceHeight;
    transform.canvasWidth = canvasWidth;
    transform.canvasHeight = canvasHeight;

    if (sourceWidth <= 0 || sourceHeight <= 0 || canvasWidth <= 0 || canvasHeight <= 0) {
        return transform;
    }

    transform.scale = std::min(static_cast<double>(canvasWidth) / sourceWidth,
                               static_cast<double>(canvasHeight) / sourceHeight);
    transform.scaledWidth =
        std::max(1, std::min(canvasWidth, static_cast<int>(sourceWidth * transform.scale)));
    transform.scaledHeight =
        std::max(1, std::min(canvasHeight, static_cast<int>(sourceHeight * transform.scale)));

    if (centred) {
        transform.offsetX = (canvasWidth - transform.scaledWidth) / 2;
        transform.offsetY = (canvasHeight - transform.scaledHeight) / 2;
    }
    return transform;
}

NormalizedBox LetterboxTransform::canvasBoxToSource(double x, double y, double width,
                                                    double height) const {
    NormalizedBox box;
    if (scale <= 0.0 || sourceWidth <= 0 || sourceHeight <= 0) return box;

    // Remove the padding, undo the scale, then normalise against the source.
    const double sourceX = (x - offsetX) / scale;
    const double sourceY = (y - offsetY) / scale;
    const double sourceW = width / scale;
    const double sourceH = height / scale;

    box.x = sourceX / sourceWidth;
    box.y = sourceY / sourceHeight;
    box.width = sourceW / sourceWidth;
    box.height = sourceH / sourceHeight;
    return box.clampedToFrame();
}

NormalizedBox LetterboxTransform::canvasCenterBoxToSource(double centerX, double centerY,
                                                          double width, double height) const {
    return canvasBoxToSource(centerX - width / 2.0, centerY - height / 2.0, width, height);
}

void letterboxToTensor(const std::uint8_t* rgb, int sourceWidth, int sourceHeight,
                       const LetterboxTransform& transform, bool channelOrderBgr, bool scaleTo01,
                       std::uint8_t padValue, std::vector<float>& output) {
    const int canvasWidth = transform.canvasWidth;
    const int canvasHeight = transform.canvasHeight;
    const std::size_t plane = static_cast<std::size_t>(canvasWidth) * canvasHeight;
    output.assign(plane * 3, static_cast<float>(padValue) / (scaleTo01 ? 255.0f : 1.0f));

    if (rgb == nullptr || sourceWidth <= 0 || sourceHeight <= 0) return;

    const double inverseScale = transform.scale > 0.0 ? 1.0 / transform.scale : 1.0;
    const float divisor = scaleTo01 ? 255.0f : 1.0f;

    for (int destinationY = 0; destinationY < transform.scaledHeight; ++destinationY) {
        // Sample the source with a half-pixel centred mapping, which keeps the
        // resized image aligned with what the box transform assumes.
        const double sourceYf =
            std::min(static_cast<double>(sourceHeight - 1),
                     std::max(0.0, (destinationY + 0.5) * inverseScale - 0.5));
        const int y0 = static_cast<int>(sourceYf);
        const int y1 = std::min(sourceHeight - 1, y0 + 1);
        const double weightY = sourceYf - y0;

        for (int destinationX = 0; destinationX < transform.scaledWidth; ++destinationX) {
            const double sourceXf =
                std::min(static_cast<double>(sourceWidth - 1),
                         std::max(0.0, (destinationX + 0.5) * inverseScale - 0.5));
            const int x0 = static_cast<int>(sourceXf);
            const int x1 = std::min(sourceWidth - 1, x0 + 1);
            const double weightX = sourceXf - x0;

            const std::size_t topLeft = (static_cast<std::size_t>(y0) * sourceWidth + x0) * 3;
            const std::size_t topRight = (static_cast<std::size_t>(y0) * sourceWidth + x1) * 3;
            const std::size_t bottomLeft = (static_cast<std::size_t>(y1) * sourceWidth + x0) * 3;
            const std::size_t bottomRight = (static_cast<std::size_t>(y1) * sourceWidth + x1) * 3;

            const std::size_t destinationIndex =
                static_cast<std::size_t>(destinationY + transform.offsetY) * canvasWidth +
                (destinationX + transform.offsetX);

            for (int channel = 0; channel < 3; ++channel) {
                const int sourceChannel = channelOrderBgr ? 2 - channel : channel;
                const double top =
                    rgb[topLeft + sourceChannel] * (1.0 - weightX) + rgb[topRight + sourceChannel] * weightX;
                const double bottom = rgb[bottomLeft + sourceChannel] * (1.0 - weightX) +
                                      rgb[bottomRight + sourceChannel] * weightX;
                const double value = top * (1.0 - weightY) + bottom * weightY;
                output[static_cast<std::size_t>(channel) * plane + destinationIndex] =
                    static_cast<float>(value) / divisor;
            }
        }
    }
}

}  // namespace trace
