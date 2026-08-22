#include "ui/common/detection_overlay.h"

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <limits>

#include "ui/common/theme.h"

namespace trace::ui {
namespace {

constexpr int kLabelPaddingX = 5;
constexpr int kLabelPaddingY = 2;

QString labelText(const DetectionOverlayItem& item, const DetectionOverlayOptions& options) {
    QString text;
    if (options.showLabels) text = item.label;
    if (options.showConfidence) {
        const QString score = QStringLiteral("%1%").arg(item.confidence * 100.0, 0, 'f', 0);
        text = text.isEmpty() ? score : text + QStringLiteral("  ") + score;
    }
    if (item.verification != DetectionVerification::Unreviewed && options.showLabels) {
        text += QStringLiteral("  · %1")
                    .arg(QString::fromUtf8(toDisplayString(item.verification)).toUpper());
    }
    return text;
}

}  // namespace

QColor detectionGroupColour(DetectionClassGroup group) {
    switch (group) {
        case DetectionClassGroup::Person:
            return colors::kDetectionPerson;
        case DetectionClassGroup::Vehicle:
            return colors::kDetectionVehicle;
        case DetectionClassGroup::Object:
            break;
    }
    return colors::kDetectionObject;
}

QColor detectionOverlayColour(DetectionClassGroup group, DetectionVerification verification) {
    switch (verification) {
        case DetectionVerification::Confirmed:
            return colors::kVerified;
        case DetectionVerification::Rejected:
            return colors::kFailure;
        case DetectionVerification::Uncertain:
            return colors::kWarning;
        case DetectionVerification::Unreviewed:
            break;
    }
    return detectionGroupColour(group);
}

QRectF detectionBoxToWidget(const NormalizedBox& box, const QRect& frameRect) {
    return QRectF(frameRect.x() + box.x * frameRect.width(),
                  frameRect.y() + box.y * frameRect.height(), box.width * frameRect.width(),
                  box.height * frameRect.height());
}

void paintDetectionOverlay(QPainter& painter, const QRect& frameRect,
                           const std::vector<DetectionOverlayItem>& items,
                           const DetectionOverlayOptions& options) {
    if (!options.showBoxes || items.empty() || frameRect.isEmpty()) return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() > 0 ? font.pointSizeF() : 9.0);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics metrics(font);

    for (const auto& item : items) {
        const QRectF box = detectionBoxToWidget(item.box, frameRect);
        if (box.width() < 1.0 || box.height() < 1.0) continue;

        const QColor colour = detectionOverlayColour(item.group, item.verification);
        QPen pen(colour, item.selected ? 3 : 2);
        // A rejected detection stays visible — it is part of the record — but is
        // drawn dashed so it cannot be mistaken for a standing result.
        if (item.verification == DetectionVerification::Rejected) pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);

        if (item.selected) {
            QColor wash = colour;
            wash.setAlpha(40);
            painter.fillRect(box, wash);
        }

        const QString text = labelText(item, options);
        if (text.isEmpty()) continue;

        const int textWidth = metrics.horizontalAdvance(text);
        const int textHeight = metrics.height();
        QRectF tag(box.left(), box.top() - textHeight - kLabelPaddingY * 2,
                   textWidth + kLabelPaddingX * 2, textHeight + kLabelPaddingY * 2);
        // Keep the tag on screen when the box touches the top of the frame.
        if (tag.top() < frameRect.top()) tag.moveTop(box.top());
        if (tag.right() > frameRect.right()) tag.moveRight(frameRect.right());

        QColor background = colour;
        background.setAlpha(215);
        painter.fillRect(tag, background);
        painter.setPen(QColor("#0a0d10"));
        painter.drawText(tag.adjusted(kLabelPaddingX, kLabelPaddingY, -kLabelPaddingX,
                                      -kLabelPaddingY),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
    painter.restore();
}

int detectionAt(const std::vector<DetectionOverlayItem>& items, const QRect& frameRect,
                const QPoint& point) {
    int best = -1;
    double bestArea = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const QRectF box = detectionBoxToWidget(items[i].box, frameRect);
        if (!box.contains(QPointF(point))) continue;
        const double area = box.width() * box.height();
        if (area < bestArea) {
            bestArea = area;
            best = static_cast<int>(i);
        }
    }
    return best;
}

}  // namespace trace::ui
