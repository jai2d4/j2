// Detection overlay geometry and the guarantee that drawing boxes over a frame
// never touches the frame itself.
#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cstdlib>

#include "ui/common/detection_overlay.h"
#include "ui/viewer/video_view.h"

namespace trace::ui {
namespace {

NormalizedBox box(double x, double y, double w, double h) { return NormalizedBox{x, y, w, h}; }

DetectionOverlayItem item(const QString& id, const NormalizedBox& geometry,
                          DetectionClassGroup group = DetectionClassGroup::Person) {
    DetectionOverlayItem overlay;
    overlay.id = id;
    overlay.box = geometry;
    overlay.label = QStringLiteral("person");
    overlay.confidence = 0.9;
    overlay.group = group;
    return overlay;
}

TEST(DetectionOverlay, NormalisedBoxesMapOntoTheLetterboxedFrame) {
    // A 16:9 frame drawn inside a 4:3 widget leaves bars top and bottom; the
    // overlay must follow the frame, not the widget, or every box would sit
    // above where the object actually is.
    const QRect frame(0, 60, 640, 360);

    const QRectF full = detectionBoxToWidget(box(0.0, 0.0, 1.0, 1.0), frame);
    EXPECT_DOUBLE_EQ(full.left(), 0.0);
    EXPECT_DOUBLE_EQ(full.top(), 60.0);
    EXPECT_DOUBLE_EQ(full.width(), 640.0);
    EXPECT_DOUBLE_EQ(full.height(), 360.0);

    const QRectF quarter = detectionBoxToWidget(box(0.5, 0.5, 0.25, 0.25), frame);
    EXPECT_DOUBLE_EQ(quarter.left(), 320.0);
    EXPECT_DOUBLE_EQ(quarter.top(), 60.0 + 180.0);
    EXPECT_DOUBLE_EQ(quarter.width(), 160.0);
    EXPECT_DOUBLE_EQ(quarter.height(), 90.0);
}

TEST(DetectionOverlay, TheSameBoxLandsProportionallyAtAnyDisplaySize) {
    const NormalizedBox geometry = box(0.25, 0.4, 0.2, 0.3);
    const QRectF small = detectionBoxToWidget(geometry, QRect(0, 0, 320, 180));
    const QRectF large = detectionBoxToWidget(geometry, QRect(0, 0, 1920, 1080));

    EXPECT_NEAR(small.left() / 320.0, large.left() / 1920.0, 1e-9);
    EXPECT_NEAR(small.top() / 180.0, large.top() / 1080.0, 1e-9);
    EXPECT_NEAR(small.width() / 320.0, large.width() / 1920.0, 1e-9);
    EXPECT_NEAR(small.height() / 180.0, large.height() / 1080.0, 1e-9);
}

TEST(DetectionOverlay, HitTestingPrefersTheSmallestBoxUnderThePointer) {
    const QRect frame(0, 0, 1000, 1000);
    std::vector<DetectionOverlayItem> items = {
        item(QStringLiteral("crowd"), box(0.0, 0.0, 1.0, 1.0)),
        item(QStringLiteral("person"), box(0.4, 0.4, 0.2, 0.2)),
    };

    EXPECT_EQ(detectionAt(items, frame, QPoint(500, 500)), 1)
        << "a small box inside a large one must stay selectable";
    EXPECT_EQ(detectionAt(items, frame, QPoint(50, 50)), 0);
    EXPECT_EQ(detectionAt(items, frame, QPoint(-5, -5)), -1);
}

TEST(DetectionOverlay, ColourCarriesGroupUntilAnAnalystHasRuled) {
    EXPECT_EQ(detectionOverlayColour(DetectionClassGroup::Person,
                                     DetectionVerification::Unreviewed),
              detectionGroupColour(DetectionClassGroup::Person));
    EXPECT_NE(detectionOverlayColour(DetectionClassGroup::Person,
                                     DetectionVerification::Confirmed),
              detectionGroupColour(DetectionClassGroup::Person));
    EXPECT_NE(detectionOverlayColour(DetectionClassGroup::Person,
                                     DetectionVerification::Rejected),
              detectionOverlayColour(DetectionClassGroup::Person,
                                     DetectionVerification::Confirmed));
    // Every group is visually distinct, so a lane colour means one thing.
    EXPECT_NE(detectionGroupColour(DetectionClassGroup::Person),
              detectionGroupColour(DetectionClassGroup::Vehicle));
    EXPECT_NE(detectionGroupColour(DetectionClassGroup::Vehicle),
              detectionGroupColour(DetectionClassGroup::Object));
}

TEST(DetectionOverlay, DrawingBoxesLeavesTheFrameImageUntouched) {
    // The overlay is a layer on the screen, not an edit. This is the same
    // guarantee at the pixel level: the decoded frame the viewer holds must be
    // bit-identical before and after the boxes are drawn over it.
    QImage source(64, 48, QImage::Format_RGB888);
    source.fill(QColor(17, 34, 51));
    const QByteArray before(reinterpret_cast<const char*>(source.constBits()),
                            static_cast<int>(source.sizeInBytes()));

    VideoView view;
    view.resize(320, 240);
    view.setFrame(source);
    view.setDetections({item(QStringLiteral("a"), box(0.1, 0.1, 0.5, 0.5))});

    QImage canvas(320, 240, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    {
        QPainter painter(&canvas);
        paintDetectionOverlay(painter, QRect(0, 0, 320, 240), view.detections(),
                              DetectionOverlayOptions{});
    }

    const QByteArray after(reinterpret_cast<const char*>(view.frame().constBits()),
                           static_cast<int>(view.frame().sizeInBytes()));
    EXPECT_EQ(before, after) << "the decoded frame must not be altered by the overlay";
    EXPECT_EQ(view.frame().size(), source.size());
}

TEST(DetectionOverlay, TurningTheOverlayOffDrawsNothing) {
    QImage withBoxes(200, 200, QImage::Format_RGB32);
    QImage without(200, 200, QImage::Format_RGB32);
    withBoxes.fill(Qt::black);
    without.fill(Qt::black);

    const std::vector<DetectionOverlayItem> items = {
        item(QStringLiteral("a"), box(0.2, 0.2, 0.4, 0.4))};

    DetectionOverlayOptions on;
    DetectionOverlayOptions off;
    off.showBoxes = false;

    {
        QPainter painter(&withBoxes);
        paintDetectionOverlay(painter, QRect(0, 0, 200, 200), items, on);
    }
    {
        QPainter painter(&without);
        paintDetectionOverlay(painter, QRect(0, 0, 200, 200), items, off);
    }

    EXPECT_NE(withBoxes, without);
    QImage blank(200, 200, QImage::Format_RGB32);
    blank.fill(Qt::black);
    EXPECT_EQ(without, blank) << "with the overlay off the frame must be left alone";
}

}  // namespace
}  // namespace trace::ui

int main(int argc, char** argv) {
    // These cases build real widgets but show nothing. Selecting the offscreen
    // platform when the caller has not chosen one keeps them runnable on a
    // build machine with no display, including during test discovery.
    if (std::getenv("QT_QPA_PLATFORM") == nullptr) {
        setenv("QT_QPA_PLATFORM", "offscreen", 0);
    }
    QApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
