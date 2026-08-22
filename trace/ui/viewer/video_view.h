#pragma once

#include <QImage>
#include <QWidget>

#include <vector>

#include "ui/common/detection_overlay.h"

namespace trace::ui {

/// Displays decoded frames, with the detection overlay drawn on top.
///
/// The image is drawn letterboxed with the aspect ratio preserved and smooth
/// scaling: an analyst must never see a frame stretched into a shape the camera
/// did not record. When no frame is available the widget says so rather than
/// showing an empty black rectangle that could be mistaken for footage.
///
/// Detection boxes are painted onto the widget, never into the frame image, so
/// what is on screen is the decoded evidence with an annotation layer over it.
/// Turning the overlay off leaves the untouched frame.
class VideoView : public QWidget {
    Q_OBJECT

public:
    explicit VideoView(QWidget* parent = nullptr);

    void setFrame(const QImage& image);
    void clear(const QString& message = {});
    void setPlaceholder(const QString& message);
    const QImage& frame() const { return image_; }

    void setDetections(std::vector<DetectionOverlayItem> detections);
    void clearDetections();
    const std::vector<DetectionOverlayItem>& detections() const { return detections_; }
    void setOverlayOptions(const DetectionOverlayOptions& options);
    const DetectionOverlayOptions& overlayOptions() const { return overlayOptions_; }
    void setSelectedDetection(const QString& detectionId);

    /// Where the frame is currently drawn. Empty when there is no frame.
    QRect frameRect() const;

    QSize sizeHint() const override;

signals:
    /// An operator clicked a detection box. Empty id means they clicked the
    /// frame away from every box.
    void detectionClicked(const QString& detectionId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QImage image_;
    QString placeholder_ = QStringLiteral("No evidence selected");
    std::vector<DetectionOverlayItem> detections_;
    DetectionOverlayOptions overlayOptions_;
    QString selectedDetectionId_;
};

}  // namespace trace::ui
