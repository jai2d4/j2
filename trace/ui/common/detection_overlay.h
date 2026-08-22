#pragma once

#include <QColor>
#include <QRect>
#include <QtGlobal>
#include <QString>

#include <vector>

#include "core/models/detection.h"

class QPainter;

namespace trace::ui {

/// One detection as the viewer draws it.
///
/// Geometry stays normalised all the way to the painter: the overlay is a view
/// of the evidence, never a change to it, and the same stored values are
/// correct in a 400-pixel panel, in full screen and in a report.
struct DetectionOverlayItem {
    QString id;
    NormalizedBox box;
    QString label;
    double confidence = 0.0;
    DetectionClassGroup group = DetectionClassGroup::Object;
    DetectionVerification verification = DetectionVerification::Unreviewed;
    bool selected = false;
};

/// What the operator has chosen to see. Display state only: hiding a box
/// changes nothing about what was stored or what the model reported.
struct DetectionOverlayOptions {
    bool showBoxes = true;
    bool showLabels = true;
    bool showConfidence = true;
};

/// The boxes belonging to one analysed frame.
///
/// The playhead rarely sits exactly on an analysed frame, so the timestamp
/// travels with the boxes: the viewer shows which moment was actually analysed
/// rather than implying the model looked at the frame on screen.
struct DetectionOverlayFrame {
    qint64 timestampUs = -1;
    std::vector<DetectionOverlayItem> items;
};

/// Lane and box colour for a class group.
QColor detectionGroupColour(DetectionClassGroup group);
/// Box colour once an analyst has reviewed the detection. Unreviewed keeps the
/// group's colour; a reviewed one takes the colour of the decision, so the
/// review state is visible on the image itself.
QColor detectionOverlayColour(DetectionClassGroup group, DetectionVerification verification);

/// Maps a normalised box onto the rectangle the frame occupies on screen.
QRectF detectionBoxToWidget(const NormalizedBox& box, const QRect& frameRect);

/// Draws the boxes inside `frameRect`, which is where the frame is displayed.
/// Nothing is drawn into the frame image itself.
void paintDetectionOverlay(QPainter& painter, const QRect& frameRect,
                           const std::vector<DetectionOverlayItem>& items,
                           const DetectionOverlayOptions& options);

/// Topmost item under `point`, or -1. Smallest-area-first so a box inside a
/// larger box is still selectable.
int detectionAt(const std::vector<DetectionOverlayItem>& items, const QRect& frameRect,
                const QPoint& point);

}  // namespace trace::ui
