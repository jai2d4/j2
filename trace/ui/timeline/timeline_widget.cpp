#include "ui/timeline/timeline_widget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "core/common/time_utils.h"
#include "ui/common/theme.h"

namespace trace::ui {
namespace {

constexpr int kRulerHeight = 24;
constexpr int kScrubHeight = 30;
constexpr int kTrackHeight = 22;
constexpr int kSideMargin = 8;
constexpr int kMarkerHalfWidth = 5;

/// Chooses a tick interval that yields readable labels at the current zoom.
qint64 chooseTickInterval(qint64 visibleUs, int pixels) {
    static const qint64 kCandidates[] = {
        100'000,      250'000,      500'000,      1'000'000,     2'000'000,     5'000'000,
        10'000'000,   15'000'000,   30'000'000,   60'000'000,    120'000'000,   300'000'000,
        600'000'000,  900'000'000,  1'800'000'000, 3'600'000'000};
    const int desiredTicks = std::max(2, pixels / 110);
    for (const qint64 candidate : kCandidates) {
        if (visibleUs / candidate <= desiredTicks) return candidate;
    }
    return 3'600'000'000;
}

}  // namespace

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(kRulerHeight + kScrubHeight + kTrackHeight * 2 + 8);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(QStringLiteral(
        "Click or drag to seek. Wheel zooms, Shift+wheel pans. Click a marker to jump to it."));
}

void TimelineWidget::setDuration(qint64 durationUs) {
    durationUs_ = std::max<qint64>(0, durationUs);
    viewStartUs_ = 0;
    zoom_ = 1.0;
    update();
}

void TimelineWidget::setPosition(qint64 positionUs) {
    positionUs_ = std::clamp<qint64>(positionUs, 0, durationUs_);
    // Keep the playhead in view while zoomed in.
    if (zoom_ > 1.0) {
        const qint64 visible = visibleDuration();
        if (positionUs_ < viewStartUs_ || positionUs_ > viewStartUs_ + visible) {
            viewStartUs_ = positionUs_ - visible / 2;
            clampView();
        }
    }
    update();
}

void TimelineWidget::setTracks(std::vector<TimelineTrack> tracks) {
    tracks_ = std::move(tracks);
    setMinimumHeight(kRulerHeight + kScrubHeight +
                     kTrackHeight * std::max<int>(2, static_cast<int>(tracks_.size())) + 8);
    update();
}

void TimelineWidget::clear() {
    durationUs_ = 0;
    positionUs_ = 0;
    viewStartUs_ = 0;
    zoom_ = 1.0;
    tracks_.clear();
    update();
}

qint64 TimelineWidget::visibleDuration() const {
    if (durationUs_ <= 0) return 0;
    return static_cast<qint64>(static_cast<double>(durationUs_) / zoom_);
}

QRect TimelineWidget::rulerRect() const {
    return QRect(kSideMargin, 0, std::max(1, width() - kSideMargin * 2), kRulerHeight);
}

QRect TimelineWidget::scrubRect() const {
    return QRect(kSideMargin, kRulerHeight, std::max(1, width() - kSideMargin * 2), kScrubHeight);
}

QRect TimelineWidget::trackRect(int index) const {
    return QRect(kSideMargin, kRulerHeight + kScrubHeight + index * kTrackHeight,
                 std::max(1, width() - kSideMargin * 2), kTrackHeight);
}

qint64 TimelineWidget::timeAtX(int x) const {
    const QRect area = scrubRect();
    if (area.width() <= 0 || durationUs_ <= 0) return 0;
    const double fraction =
        std::clamp(static_cast<double>(x - area.left()) / static_cast<double>(area.width()), 0.0, 1.0);
    return viewStartUs_ + static_cast<qint64>(fraction * static_cast<double>(visibleDuration()));
}

double TimelineWidget::xAtTime(qint64 timeUs) const {
    const QRect area = scrubRect();
    const qint64 visible = visibleDuration();
    if (visible <= 0) return area.left();
    const double fraction = static_cast<double>(timeUs - viewStartUs_) / static_cast<double>(visible);
    return area.left() + fraction * area.width();
}

void TimelineWidget::clampView() {
    const qint64 visible = visibleDuration();
    viewStartUs_ = std::clamp<qint64>(viewStartUs_, 0, std::max<qint64>(0, durationUs_ - visible));
}

void TimelineWidget::applyZoom(double factor, int anchorX) {
    if (durationUs_ <= 0) return;
    const qint64 anchorTime = timeAtX(anchorX);
    zoom_ = std::clamp(zoom_ * factor, 1.0, 2000.0);
    const qint64 visible = visibleDuration();
    const QRect area = scrubRect();
    const double fraction =
        area.width() > 0
            ? std::clamp(static_cast<double>(anchorX - area.left()) / area.width(), 0.0, 1.0)
            : 0.5;
    viewStartUs_ = anchorTime - static_cast<qint64>(fraction * static_cast<double>(visible));
    clampView();
    update();
}

void TimelineWidget::zoomIn() { applyZoom(1.5, width() / 2); }
void TimelineWidget::zoomOut() { applyZoom(1.0 / 1.5, width() / 2); }
void TimelineWidget::resetZoom() {
    zoom_ = 1.0;
    viewStartUs_ = 0;
    update();
}

std::optional<std::pair<int, int>> TimelineWidget::markerAt(const QPoint& point) const {
    for (int trackIndex = 0; trackIndex < static_cast<int>(tracks_.size()); ++trackIndex) {
        const QRect area = trackRect(trackIndex);
        if (!area.contains(point)) continue;
        const auto& markers = tracks_[static_cast<std::size_t>(trackIndex)].markers;
        for (int markerIndex = 0; markerIndex < static_cast<int>(markers.size()); ++markerIndex) {
            const auto& marker = markers[static_cast<std::size_t>(markerIndex)];
            const double startX = xAtTime(marker.startUs);
            const double endX = marker.endUs ? xAtTime(*marker.endUs) : startX;
            if (point.x() >= startX - kMarkerHalfWidth && point.x() <= endX + kMarkerHalfWidth) {
                return std::make_pair(trackIndex, markerIndex);
            }
        }
    }
    return std::nullopt;
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), colors::kSurface);

    if (durationUs_ <= 0) {
        painter.setPen(colors::kTextDisabled);
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("No media loaded — the timeline appears when evidence is "
                                        "opened in the viewer"));
        return;
    }

    const QRect ruler = rulerRect();
    const QRect scrub = scrubRect();
    const qint64 visible = visibleDuration();

    // Ruler
    painter.setPen(QPen(colors::kBorder, 1));
    painter.drawLine(ruler.left(), ruler.bottom(), ruler.right(), ruler.bottom());

    const qint64 interval = chooseTickInterval(visible, scrub.width());
    const qint64 firstTick = (viewStartUs_ / interval) * interval;
    QFont tickFont = painter.font();
    tickFont.setPointSizeF(std::max(7.5, tickFont.pointSizeF() - 2.0));
    painter.setFont(tickFont);

    for (qint64 tick = firstTick; tick <= viewStartUs_ + visible; tick += interval) {
        if (tick < 0) continue;
        const int x = static_cast<int>(std::lround(xAtTime(tick)));
        if (x < scrub.left() - 2 || x > scrub.right() + 2) continue;
        painter.setPen(QPen(colors::kBorderStrong, 1));
        painter.drawLine(x, ruler.bottom() - 6, x, ruler.bottom());
        painter.setPen(colors::kTextSecondary);
        const bool withMillis = interval < 1'000'000;
        painter.drawText(QRect(x + 3, ruler.top(), 92, ruler.height() - 4),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::fromStdString(formatTimecode(tick, withMillis)));
    }

    // Scrub area
    painter.fillRect(scrub, colors::kBackground);
    painter.setPen(QPen(colors::kBorder, 1));
    painter.drawRect(scrub.adjusted(0, 0, -1, -1));

    // Elapsed portion
    const double playheadX = xAtTime(positionUs_);
    if (playheadX > scrub.left()) {
        QRect elapsed(scrub.left() + 1, scrub.top() + 1,
                      std::min<int>(static_cast<int>(playheadX) - scrub.left() - 1, scrub.width() - 2),
                      scrub.height() - 2);
        painter.fillRect(elapsed, QColor(colors::kAccentMuted.red(), colors::kAccentMuted.green(),
                                         colors::kAccentMuted.blue(), 90));
    }

    // Tracks
    for (int index = 0; index < static_cast<int>(tracks_.size()); ++index) {
        const auto& track = tracks_[static_cast<std::size_t>(index)];
        const QRect area = trackRect(index);
        painter.setPen(colors::kTextDisabled);
        QFont labelFont = painter.font();
        labelFont.setPointSizeF(std::max(7.0, labelFont.pointSizeF()));
        painter.setFont(labelFont);
        painter.drawText(QRect(area.left() + 2, area.top(), 90, area.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, track.name);

        if (track.isEnvelope()) {
            // The envelope spans the whole item, so each bucket maps to a moment and
            // then to an x position — which means it stays correct under zoom and
            // scroll without being rebuilt.
            const QRect plot(area.left() + 94, area.top() + 2, area.right() - area.left() - 96,
                             area.height() - 4);
            if (plot.width() > 0 && durationUs_ > 0) {
                const int midline = plot.center().y();
                painter.setPen(QPen(QColor(track.colour.red(), track.colour.green(),
                                           track.colour.blue(), 60), 1));
                painter.drawLine(plot.left(), midline, plot.right(), midline);

                const auto bucketCount = track.envelopePeaks.size();
                QColor peakColour = track.colour;
                peakColour.setAlpha(120);
                QColor rmsColour = track.colour;
                rmsColour.setAlpha(220);

                for (int px = plot.left(); px <= plot.right(); ++px) {
                    const qint64 at = timeAtX(px);
                    if (at < 0 || at > durationUs_) continue;
                    auto bucket = static_cast<std::size_t>(
                        static_cast<double>(at) / static_cast<double>(durationUs_) *
                        static_cast<double>(bucketCount));
                    if (bucket >= bucketCount) bucket = bucketCount - 1;

                    const int peakHeight = static_cast<int>(
                        static_cast<double>(track.envelopePeaks[bucket]) * (plot.height() / 2.0));
                    if (peakHeight > 0) {
                        painter.setPen(peakColour);
                        painter.drawLine(px, midline - peakHeight, px, midline + peakHeight);
                    }
                    if (bucket < track.envelopeRms.size()) {
                        const int rmsHeight = static_cast<int>(
                            static_cast<double>(track.envelopeRms[bucket]) * (plot.height() / 2.0));
                        if (rmsHeight > 0) {
                            painter.setPen(rmsColour);
                            painter.drawLine(px, midline - rmsHeight, px, midline + rmsHeight);
                        }
                    }
                }
            }
            continue;
        }

        for (const auto& marker : track.markers) {
            const double startX = xAtTime(marker.startUs);
            if (marker.endUs) {
                const double endX = xAtTime(*marker.endUs);
                QRect bar(static_cast<int>(startX), area.top() + 5,
                          std::max(3, static_cast<int>(endX - startX)), area.height() - 10);
                QColor fill = marker.colour;
                fill.setAlpha(150);
                painter.fillRect(bar, fill);
                painter.setPen(QPen(marker.colour, 1));
                painter.drawRect(bar);
            } else {
                const int x = static_cast<int>(std::lround(startX));
                if (x < area.left() - 4 || x > area.right() + 4) continue;
                QPolygon pin;
                pin << QPoint(x, area.top() + area.height() - 4)
                    << QPoint(x - kMarkerHalfWidth, area.top() + 4)
                    << QPoint(x + kMarkerHalfWidth, area.top() + 4);
                painter.setPen(QPen(marker.colour, 1));
                painter.setBrush(marker.colour);
                painter.drawPolygon(pin);
                painter.setBrush(Qt::NoBrush);
            }
        }
    }

    // Playhead over everything
    const int x = static_cast<int>(std::lround(playheadX));
    if (x >= scrub.left() && x <= scrub.right()) {
        painter.setPen(QPen(colors::kPlayhead, 1));
        painter.drawLine(x, ruler.bottom(), x, height() - 2);
        QPolygon head;
        head << QPoint(x - 5, ruler.bottom() + 1) << QPoint(x + 5, ruler.bottom() + 1)
             << QPoint(x, ruler.bottom() + 8);
        painter.setBrush(colors::kPlayhead);
        painter.drawPolygon(head);
        painter.setBrush(Qt::NoBrush);
    }

    // Current position and zoom, bottom right
    painter.setPen(colors::kTextSecondary);
    const QString status =
        zoom_ > 1.01
            ? QStringLiteral("%1  ·  zoom %2x")
                  .arg(QString::fromStdString(formatTimecode(positionUs_)))
                  .arg(zoom_, 0, 'f', 1)
            : QString::fromStdString(formatTimecode(positionUs_));
    painter.drawText(QRect(width() - 210, height() - 18, 200, 16),
                     Qt::AlignRight | Qt::AlignVCenter, status);
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (durationUs_ <= 0 || event->button() != Qt::LeftButton) return;

    if (const auto hit = markerAt(event->pos()); hit) {
        const auto& track = tracks_[static_cast<std::size_t>(hit->first)];
        const auto& marker = track.markers[static_cast<std::size_t>(hit->second)];
        emit seekRequested(marker.startUs);
        emit markerActivated(track.name, marker.id);
        return;
    }

    if (event->pos().y() <= rulerRect().bottom() + kScrubHeight) {
        dragging_ = true;
        emit seekRequested(timeAtX(event->pos().x()));
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        emit seekRequested(timeAtX(event->pos().x()));
        return;
    }
    if (const auto hit = markerAt(event->pos()); hit) {
        const auto& marker =
            tracks_[static_cast<std::size_t>(hit->first)].markers[static_cast<std::size_t>(hit->second)];
        setCursor(Qt::PointingHandCursor);
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("%1\n%2")
                               .arg(QString::fromStdString(formatTimecode(marker.startUs)),
                                    marker.label),
                           this);
        return;
    }
    setCursor(event->pos().y() <= rulerRect().bottom() + kScrubHeight ? Qt::SizeHorCursor
                                                                     : Qt::ArrowCursor);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void TimelineWidget::wheelEvent(QWheelEvent* event) {
    if (durationUs_ <= 0) return;
    const int delta = event->angleDelta().y();
    if (delta == 0) return;

    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        const qint64 step = visibleDuration() / 8;
        viewStartUs_ += delta > 0 ? -step : step;
        clampView();
        update();
        event->accept();
        return;
    }
    applyZoom(delta > 0 ? 1.25 : 1.0 / 1.25, static_cast<int>(event->position().x()));
    event->accept();
}

void TimelineWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    clampView();
}

}  // namespace trace::ui
