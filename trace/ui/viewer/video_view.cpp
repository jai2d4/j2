#include "ui/viewer/video_view.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>

#include "ui/common/theme.h"

namespace trace::ui {

VideoView::VideoView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
}

QSize VideoView::sizeHint() const { return QSize(960, 540); }

void VideoView::setFrame(const QImage& image) {
    image_ = image;
    update();
}

void VideoView::clear(const QString& message) {
    image_ = QImage();
    detections_.clear();
    if (!message.isEmpty()) placeholder_ = message;
    update();
}

void VideoView::setPlaceholder(const QString& message) {
    placeholder_ = message;
    if (image_.isNull()) update();
}

void VideoView::setDetections(std::vector<DetectionOverlayItem> detections) {
    detections_ = std::move(detections);
    for (auto& item : detections_) item.selected = !selectedDetectionId_.isEmpty() &&
                                                   item.id == selectedDetectionId_;
    update();
}

void VideoView::clearDetections() {
    if (detections_.empty()) return;
    detections_.clear();
    update();
}

void VideoView::setOverlayOptions(const DetectionOverlayOptions& options) {
    overlayOptions_ = options;
    update();
}

void VideoView::setSelectedDetection(const QString& detectionId) {
    selectedDetectionId_ = detectionId;
    for (auto& item : detections_) item.selected = !detectionId.isEmpty() && item.id == detectionId;
    update();
}

QRect VideoView::frameRect() const {
    if (image_.isNull()) return {};
    const QSize scaled = image_.size().scaled(size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled);
}

void VideoView::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.fillRect(event->rect(), QColor("#05070a"));

    if (image_.isNull()) {
        painter.setPen(colors::kTextDisabled);
        QFont font = painter.font();
        if (font.pointSize() > 0) font.setPointSize(font.pointSize() + 1);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, placeholder_);
        return;
    }

    const QRect target = frameRect();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, image_);

    paintDetectionOverlay(painter, target, detections_, overlayOptions_);

    painter.setPen(QPen(colors::kBorder, 1));
    painter.drawRect(target.adjusted(0, 0, -1, -1));
}

void VideoView::mousePressEvent(QMouseEvent* event) {
    const QRect target = frameRect();
    if (event->button() != Qt::LeftButton || target.isEmpty() || !overlayOptions_.showBoxes) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int index = detectionAt(detections_, target, event->pos());
    if (index < 0) {
        if (target.contains(event->pos())) emit detectionClicked(QString());
        QWidget::mousePressEvent(event);
        return;
    }
    emit detectionClicked(detections_[static_cast<std::size_t>(index)].id);
}

}  // namespace trace::ui
