#pragma once

#include <QImage>
#include <QWidget>

namespace trace::ui {

/// Displays decoded frames.
///
/// The image is drawn letterboxed with the aspect ratio preserved and smooth
/// scaling: an analyst must never see a frame stretched into a shape the camera
/// did not record. When no frame is available the widget says so rather than
/// showing an empty black rectangle that could be mistaken for footage.
class VideoView : public QWidget {
    Q_OBJECT

public:
    explicit VideoView(QWidget* parent = nullptr);

    void setFrame(const QImage& image);
    void clear(const QString& message = {});
    void setPlaceholder(const QString& message);
    const QImage& frame() const { return image_; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;
    QString placeholder_ = QStringLiteral("No evidence selected");
};

}  // namespace trace::ui
