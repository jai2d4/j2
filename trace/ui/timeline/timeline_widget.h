#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

namespace trace::ui {

/// One marked item on a timeline track: a moment or a span.
struct TimelineMarker {
    QString id;
    qint64 startUs = 0;
    std::optional<qint64> endUs;  ///< spans draw as a bar, moments as a pin
    QString label;
    QColor colour;
};

/// A row of markers. Phase 0 fills two — bookmarks and annotations — but the
/// widget knows nothing about either: later phases add detection, event, audio
/// or multi-camera tracks by pushing more of these, with no change here.
struct TimelineTrack {
    QString name;
    QColor colour;
    std::vector<TimelineMarker> markers;
};

/// The case timeline: duration, playhead, and one row per marker track.
///
/// Time is drawn from real microsecond positions, so a variable-frame-rate
/// recording lands where its timestamps say and not where a frame count would
/// guess.
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setDuration(qint64 durationUs);
    void setPosition(qint64 positionUs);
    void setTracks(std::vector<TimelineTrack> tracks);
    void clear();

    qint64 duration() const { return durationUs_; }
    qint64 position() const { return positionUs_; }
    const std::vector<TimelineTrack>& tracks() const { return tracks_; }

    void zoomIn();
    void zoomOut();
    void resetZoom();

signals:
    void seekRequested(qint64 positionUs);
    void markerActivated(const QString& trackName, const QString& markerId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    qint64 visibleDuration() const;
    qint64 timeAtX(int x) const;
    double xAtTime(qint64 timeUs) const;
    void clampView();
    void applyZoom(double factor, int anchorX);
    std::optional<std::pair<int, int>> markerAt(const QPoint& point) const;
    QRect trackRect(int index) const;
    QRect rulerRect() const;
    QRect scrubRect() const;

    qint64 durationUs_ = 0;
    qint64 positionUs_ = 0;
    qint64 viewStartUs_ = 0;
    double zoom_ = 1.0;  ///< 1.0 shows the whole item
    bool dragging_ = false;
    int hoveredTrack_ = -1;
    int hoveredMarker_ = -1;
    std::vector<TimelineTrack> tracks_;
};

}  // namespace trace::ui
