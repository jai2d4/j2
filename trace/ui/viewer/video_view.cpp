#include "ui/viewer/video_view.h"

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
    if (!message.isEmpty()) placeholder_ = message;
    update();
}

void VideoView::setPlaceholder(const QString& message) {
    placeholder_ = message;
    if (image_.isNull()) update();
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

    const QSize scaled = image_.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2),
                       scaled);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, image_);

    painter.setPen(QPen(colors::kBorder, 1));
    painter.drawRect(target.adjusted(0, 0, -1, -1));
}

}  // namespace trace::ui
