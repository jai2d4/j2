// Phase 1 unit tests: the pure pieces of the detection path — coordinate
// transforms, suppression, model-output decoding, model validation and the
// provider abstraction. None of these need a model, a GPU or a video.
#include <gtest/gtest.h>

#include <cmath>
#include <fstream>

#include "ai/detection/detection_provider_registry.h"
#include "ai/detection/models/class_taxonomy.h"
#include "ai/detection/models/model_manager.h"
#include "ai/detection/postprocessing/nms.h"
#include "ai/detection/preprocessing/letterbox.h"
#include "ai/detection/providers/mock_detection_provider.h"
#include "analysis/frame_sampler.h"
#include "core/models/analysis_run.h"
#include "core/models/detection.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

// ------------------------------------------------------------------ geometry

TEST(NormalizedBox, ValidityAndClamping) {
    NormalizedBox inside{0.1, 0.2, 0.3, 0.4};
    EXPECT_TRUE(inside.valid());
    EXPECT_NEAR(inside.right(), 0.4, 1e-9);
    EXPECT_NEAR(inside.bottom(), 0.6, 1e-9);
    EXPECT_NEAR(inside.area(), 0.12, 1e-9);

    // A box hanging off the right and bottom edges is trimmed, not reported
    // with coordinates that cannot exist in the frame.
    const NormalizedBox overhanging{0.8, 0.9, 0.5, 0.4};
    const NormalizedBox clamped = overhanging.clampedToFrame();
    EXPECT_NEAR(clamped.x, 0.8, 1e-9);
    EXPECT_NEAR(clamped.right(), 1.0, 1e-9);
    EXPECT_NEAR(clamped.bottom(), 1.0, 1e-9);
    EXPECT_TRUE(clamped.valid());

    // A box entirely outside collapses to nothing rather than inverting.
    const NormalizedBox outside = NormalizedBox{1.5, 1.5, 0.2, 0.2}.clampedToFrame();
    EXPECT_NEAR(outside.width, 0.0, 1e-9);
    EXPECT_FALSE(outside.valid());
}

TEST(Letterbox, ScalesToFitAndKeepsAspectRatio) {
    // 768x576 (4:3) into a 416x416 square: width-limited, so the image occupies
    // the full width and part of the height.
    const LetterboxTransform transform = computeLetterbox(768, 576, 416, 416);
    EXPECT_NEAR(transform.scale, 416.0 / 768.0, 1e-9);
    EXPECT_EQ(transform.scaledWidth, 416);
    EXPECT_EQ(transform.scaledHeight, 312);
    EXPECT_EQ(transform.offsetX, 0);
    EXPECT_EQ(transform.offsetY, 0);
}

TEST(Letterbox, CentredPlacementPadsBothSides) {
    const LetterboxTransform transform = computeLetterbox(768, 576, 416, 416, /*centred=*/true);
    EXPECT_EQ(transform.offsetX, 0);
    EXPECT_EQ(transform.offsetY, (416 - 312) / 2);
}

TEST(Letterbox, MapsCanvasBoxesBackToSourceCoordinates) {
    const LetterboxTransform transform = computeLetterbox(768, 576, 416, 416);

    // A box covering the whole scaled image must map to the whole frame.
    const NormalizedBox full =
        transform.canvasBoxToSource(0, 0, transform.scaledWidth, transform.scaledHeight);
    EXPECT_NEAR(full.x, 0.0, 1e-6);
    EXPECT_NEAR(full.y, 0.0, 1e-6);
    EXPECT_NEAR(full.width, 1.0, 1e-3);
    EXPECT_NEAR(full.height, 1.0, 1e-3);

    // A known quarter-position box round-trips to the same fraction.
    const double halfWidth = transform.scaledWidth / 2.0;
    const double halfHeight = transform.scaledHeight / 2.0;
    const NormalizedBox quarter = transform.canvasBoxToSource(0, 0, halfWidth, halfHeight);
    EXPECT_NEAR(quarter.width, 0.5, 1e-6);
    EXPECT_NEAR(quarter.height, 0.5, 1e-6);
}

TEST(Letterbox, CentredPaddingIsRemovedFromMappedBoxes) {
    const LetterboxTransform transform = computeLetterbox(768, 576, 416, 416, /*centred=*/true);
    // A box at the very top of the scaled image sits below the padding on the
    // canvas; mapping it back must land at y = 0 in the source.
    const NormalizedBox box =
        transform.canvasBoxToSource(transform.offsetX, transform.offsetY, 40, 40);
    EXPECT_NEAR(box.x, 0.0, 1e-6);
    EXPECT_NEAR(box.y, 0.0, 1e-6);
}

TEST(Letterbox, CentreFormBoxesConvertToCorners) {
    const LetterboxTransform transform = computeLetterbox(400, 400, 400, 400);
    const NormalizedBox box = transform.canvasCenterBoxToSource(200, 200, 100, 50);
    EXPECT_NEAR(box.x, 150.0 / 400.0, 1e-6);
    EXPECT_NEAR(box.y, 175.0 / 400.0, 1e-6);
    EXPECT_NEAR(box.width, 100.0 / 400.0, 1e-6);
    EXPECT_NEAR(box.height, 50.0 / 400.0, 1e-6);
}

TEST(Letterbox, TensorIsPaddedAndOrderedForTheModel) {
    // A 2x1 red/blue image into a 4x4 canvas: check padding value, channel
    // order and NCHW layout without involving a model.
    const std::vector<std::uint8_t> rgb = {255, 0, 0, 0, 0, 255};
    const LetterboxTransform transform = computeLetterbox(2, 1, 4, 4);
    std::vector<float> tensor;
    letterboxToTensor(rgb.data(), 2, 1, transform, /*channelOrderBgr=*/true, /*scaleTo01=*/false,
                      114, tensor);

    ASSERT_EQ(tensor.size(), 4u * 4u * 3u);
    const std::size_t plane = 16;
    // BGR ordering means the first plane is blue: the left pixel is pure red,
    // so its blue channel is 0 and its red channel (third plane) is 255.
    EXPECT_NEAR(tensor[0], 0.0f, 1.0f);
    EXPECT_NEAR(tensor[2 * plane], 255.0f, 1.0f);
    // Padding beyond the scaled image keeps the model's pad value.
    EXPECT_NEAR(tensor[plane - 1], 114.0f, 0.001f);
}

// ----------------------------------------------------------------- suppression

TEST(NonMaximumSuppression, IouIsSymmetricAndBounded) {
    const NormalizedBox a{0.0, 0.0, 0.5, 0.5};
    const NormalizedBox b{0.25, 0.25, 0.5, 0.5};
    EXPECT_NEAR(intersectionOverUnion(a, a), 1.0, 1e-9);
    EXPECT_NEAR(intersectionOverUnion(a, b), intersectionOverUnion(b, a), 1e-9);
    EXPECT_GT(intersectionOverUnion(a, b), 0.0);
    EXPECT_LT(intersectionOverUnion(a, b), 1.0);
    EXPECT_NEAR(intersectionOverUnion(a, NormalizedBox{0.9, 0.9, 0.1, 0.1}), 0.0, 1e-9);
}

RawDetection makeRaw(const std::string& label, int classId, double confidence,
                     const NormalizedBox& box) {
    RawDetection detection;
    detection.classId = classId;
    detection.classLabel = label;
    detection.classGroup = classGroupForLabel(label);
    detection.confidence = confidence;
    detection.box = box;
    return detection;
}

TEST(NonMaximumSuppression, KeepsTheStrongestOfOverlappingSameClassBoxes) {
    std::vector<RawDetection> input = {
        makeRaw("person", 0, 0.70, {0.10, 0.10, 0.20, 0.40}),
        makeRaw("person", 0, 0.95, {0.11, 0.11, 0.20, 0.40}),
        makeRaw("person", 0, 0.60, {0.60, 0.10, 0.20, 0.40}),
    };
    const auto kept = nonMaximumSuppression(input, 0.45, 100);
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_NEAR(kept[0].confidence, 0.95, 1e-9);  // sorted by confidence
    EXPECT_NEAR(kept[1].confidence, 0.60, 1e-9);
}

TEST(NonMaximumSuppression, DoesNotSuppressAcrossClasses) {
    // A person standing in front of a car overlaps it; both are real.
    std::vector<RawDetection> input = {
        makeRaw("person", 0, 0.90, {0.10, 0.10, 0.30, 0.60}),
        makeRaw("car", 2, 0.80, {0.11, 0.11, 0.30, 0.60}),
    };
    const auto kept = nonMaximumSuppression(input, 0.10, 100);
    EXPECT_EQ(kept.size(), 2u);
}

TEST(NonMaximumSuppression, RespectsTheKeepLimit) {
    std::vector<RawDetection> input;
    for (int i = 0; i < 10; ++i) {
        input.push_back(makeRaw("person", 0, 0.5 + i * 0.01,
                                {0.05 * i, 0.0, 0.04, 0.04}));
    }
    EXPECT_EQ(nonMaximumSuppression(input, 0.45, 3).size(), 3u);
}

// --------------------------------------------------------------- model output

TEST(YoloxDecode, ConvertsGridOutputIntoSourceCoordinates) {
    // A synthetic tensor with exactly one active anchor, so the expected box can
    // be computed by hand and compared exactly.
    constexpr int kCanvas = 416;
    const std::vector<int> strides = {8, 16, 32};
    const int anchors = 52 * 52 + 26 * 26 + 13 * 13;
    ASSERT_EQ(anchors, 3549);
    const int values = 85;

    std::vector<float> tensor(static_cast<std::size_t>(anchors) * values, 0.0f);
    // Third stride block (32), grid row 3, column 4.
    const int anchorIndex = 52 * 52 + 26 * 26 + (3 * 13 + 4);
    float* anchor = tensor.data() + static_cast<std::size_t>(anchorIndex) * values;
    anchor[0] = 0.5f;                    // dx within the cell
    anchor[1] = 0.5f;                    // dy within the cell
    anchor[2] = std::log(2.0f);          // width  = 2 * stride = 64
    anchor[3] = 0.0f;                    // height = 1 * stride = 32
    anchor[4] = 0.9f;                    // objectness
    anchor[5] = 0.8f;                    // class 0 ("person")

    const LetterboxTransform transform = computeLetterbox(kCanvas, kCanvas, kCanvas, kCanvas);
    const auto detections = decodeYoloxOutput(tensor.data(), anchors, values, kCanvas, kCanvas,
                                              strides, cocoClassLabels(), 0.5, transform);

    ASSERT_EQ(detections.size(), 1u);
    const RawDetection& detection = detections.front();
    EXPECT_EQ(detection.classLabel, "person");
    EXPECT_EQ(detection.classGroup, DetectionClassGroup::Person);
    EXPECT_NEAR(detection.confidence, 0.9 * 0.8, 1e-6);

    // centre = ((0.5 + 4) * 32, (0.5 + 3) * 32) = (144, 112); size = (64, 32)
    EXPECT_NEAR(detection.box.x, 112.0 / kCanvas, 1e-6);
    EXPECT_NEAR(detection.box.y, 96.0 / kCanvas, 1e-6);
    EXPECT_NEAR(detection.box.width, 64.0 / kCanvas, 1e-6);
    EXPECT_NEAR(detection.box.height, 32.0 / kCanvas, 1e-6);
}

TEST(YoloxDecode, DropsAnchorsBelowTheConfidenceThreshold) {
    constexpr int kCanvas = 416;
    const int anchors = 3549;
    const int values = 85;
    std::vector<float> tensor(static_cast<std::size_t>(anchors) * values, 0.0f);
    float* anchor = tensor.data();
    anchor[4] = 0.4f;
    anchor[5] = 0.4f;  // combined score 0.16

    const LetterboxTransform transform = computeLetterbox(kCanvas, kCanvas, kCanvas, kCanvas);
    EXPECT_TRUE(decodeYoloxOutput(tensor.data(), anchors, values, kCanvas, kCanvas, {8, 16, 32},
                                  cocoClassLabels(), 0.5, transform)
                    .empty());
    EXPECT_EQ(decodeYoloxOutput(tensor.data(), anchors, values, kCanvas, kCanvas, {8, 16, 32},
                                cocoClassLabels(), 0.1, transform)
                  .size(),
              1u);
}

// ------------------------------------------------------------------ taxonomy

TEST(ClassTaxonomy, GroupsObservableClassesWithoutInterpretation) {
    EXPECT_EQ(cocoClassLabels().size(), 80u);
    EXPECT_EQ(classGroupForLabel("person"), DetectionClassGroup::Person);
    for (const char* vehicle : {"car", "truck", "bus", "motorcycle", "bicycle", "train"}) {
        EXPECT_EQ(classGroupForLabel(vehicle), DetectionClassGroup::Vehicle) << vehicle;
    }
    EXPECT_EQ(classGroupForLabel("backpack"), DetectionClassGroup::Object);
    // An unknown label is still usable output, grouped conservatively.
    EXPECT_EQ(classGroupForLabel("wombat"), DetectionClassGroup::Object);

    EXPECT_EQ(labelForClassId(cocoClassLabels(), 0), "person");
    EXPECT_EQ(labelForClassId(cocoClassLabels(), 2), "car");
    // Out of range never invents a name.
    EXPECT_EQ(labelForClassId(cocoClassLabels(), 999), "class_999");
}

// ------------------------------------------------------------- model manager

TEST(ModelManager, DescribesCatalogueEntries) {
    const auto descriptor = ModelManager::describe("yolox-tiny");
    ASSERT_TRUE(descriptor.has_value());
    EXPECT_EQ(descriptor->family, "yolox");
    EXPECT_EQ(descriptor->inputWidth, 416);
    EXPECT_EQ(descriptor->inputHeight, 416);
    EXPECT_FALSE(descriptor->performsOwnNms);
    EXPECT_EQ(descriptor->classes.size(), 80u);
    EXPECT_FALSE(descriptor->licence.empty()) << "a bundled model must state its licence";
    EXPECT_FALSE(descriptor->source.empty());
    EXPECT_FALSE(ModelManager::describe("no-such-model").has_value());
}

TEST(ModelManager, ReportsAMissingModelRatherThanFetchingIt) {
    testing::TemporaryDirectory directory("trace-models");
    ModelManager manager(directory.path());

    const auto descriptor = ModelManager::describe("yolox-tiny");
    ASSERT_TRUE(descriptor.has_value());
    auto validation = manager.validate(*descriptor);
    ASSERT_TRUE(validation.ok());
    EXPECT_FALSE(validation.value().present);
    EXPECT_FALSE(validation.value().usable());
    EXPECT_NE(validation.value().problem.find("not found"), std::string::npos);
    EXPECT_TRUE(manager.installed().empty());
    // Nothing was downloaded behind the operator's back.
    EXPECT_FALSE(std::filesystem::exists(manager.pathFor(*descriptor)));
}

TEST(ModelManager, RefusesAFileWhoseDigestDoesNotMatch) {
    testing::TemporaryDirectory directory("trace-models");
    ModelManager manager(directory.path());
    const auto descriptor = ModelManager::describe("yolox-tiny");
    ASSERT_TRUE(descriptor.has_value());

    // A file of the right name but the wrong content: same name, different
    // artefact, which is exactly what the digest exists to catch.
    { std::ofstream(manager.pathFor(*descriptor), std::ios::binary) << "not the real model"; }

    auto validation = manager.validate(*descriptor);
    ASSERT_TRUE(validation.ok());
    EXPECT_TRUE(validation.value().present);
    EXPECT_FALSE(validation.value().checksumMatches);
    EXPECT_FALSE(validation.value().usable());
    EXPECT_EQ(validation.value().sha256.size(), 64u);
    EXPECT_NE(validation.value().problem.find("does not match"), std::string::npos);
}

TEST(ModelManager, HonoursAnEnvironmentOverrideForTheModelDirectory) {
    EXPECT_EQ(ModelManager::defaultModelDirectory("/data/TRACE"),
              std::filesystem::path("/data/TRACE/models"));
}

// ------------------------------------------------------------ run lifecycle

TEST(AnalysisRunStatusRules, TerminalAndCompleteAreDistinct) {
    EXPECT_FALSE(isTerminal(AnalysisRunStatus::Queued));
    EXPECT_FALSE(isTerminal(AnalysisRunStatus::Running));
    EXPECT_TRUE(isTerminal(AnalysisRunStatus::Completed));
    EXPECT_TRUE(isTerminal(AnalysisRunStatus::Cancelled));
    EXPECT_TRUE(isTerminal(AnalysisRunStatus::Failed));
    EXPECT_TRUE(isTerminal(AnalysisRunStatus::Partial));

    // Only a completed run may be presented as complete.
    EXPECT_TRUE(producedCompleteResults(AnalysisRunStatus::Completed));
    EXPECT_FALSE(producedCompleteResults(AnalysisRunStatus::Partial));
    EXPECT_FALSE(producedCompleteResults(AnalysisRunStatus::Cancelled));
    EXPECT_FALSE(producedCompleteResults(AnalysisRunStatus::Failed));

    for (const auto status : {AnalysisRunStatus::Queued, AnalysisRunStatus::Running,
                              AnalysisRunStatus::Completed, AnalysisRunStatus::Cancelled,
                              AnalysisRunStatus::Failed, AnalysisRunStatus::Partial}) {
        EXPECT_EQ(analysisRunStatusFromString(toString(status)), status);
    }
}

TEST(DetectionVerificationRules, RoundTripAndDefault) {
    for (const auto state : {DetectionVerification::Unreviewed, DetectionVerification::Confirmed,
                             DetectionVerification::Rejected, DetectionVerification::Uncertain}) {
        EXPECT_EQ(detectionVerificationFromString(toString(state)), state);
    }
    EXPECT_EQ(detectionVerificationFromString("nonsense"), DetectionVerification::Unreviewed);
    EXPECT_EQ(Detection{}.verification, DetectionVerification::Unreviewed)
        << "machine output must start unreviewed";
}

// ------------------------------------------------------------- frame sampling

TEST(FrameSampler, SamplesByTimestampNotByFrameCount) {
    // Deliberately irregular timestamps, as a variable-frame-rate recording
    // produces. Sampling at 10/s must follow the clock, not the frame index.
    FrameSampler sampler(samplingIntervalUs(AnalysisQuality::Standard));
    const std::vector<Microseconds> timestamps = {0,       10'000,  90'000,  105'000, 130'000,
                                                  205'000, 260'000, 900'000, 905'000};
    std::vector<Microseconds> accepted;
    for (const auto timestamp : timestamps) {
        if (sampler.accept(timestamp)) accepted.push_back(timestamp);
    }
    ASSERT_FALSE(accepted.empty());
    EXPECT_EQ(accepted.front(), 0);
    for (std::size_t i = 1; i < accepted.size(); ++i) {
        EXPECT_GE(accepted[i] - accepted[i - 1], 100'000)
            << "accepted frames must be at least one interval apart in media time";
    }
    EXPECT_NE(std::find(accepted.begin(), accepted.end(), 900'000), accepted.end())
        << "a frame after a long gap must be analysed";
}

TEST(FrameSampler, DetailedQualityAnalysesEveryFrame) {
    FrameSampler sampler(samplingIntervalUs(AnalysisQuality::Detailed));
    EXPECT_EQ(sampler.intervalUs(), 0);
    for (const Microseconds timestamp : {0, 1, 2, 3, 40'000}) {
        EXPECT_TRUE(sampler.accept(timestamp));
    }
}

TEST(FrameSampler, QualityLevelsMapToDocumentedRates) {
    EXPECT_EQ(samplingIntervalUs(AnalysisQuality::Fast), 200'000);      // ~5/s
    EXPECT_EQ(samplingIntervalUs(AnalysisQuality::Standard), 100'000);  // ~10/s
    EXPECT_EQ(samplingIntervalUs(AnalysisQuality::Detailed), 0);
    EXPECT_EQ(samplingIntervalUs(AnalysisQuality::Custom, 33'333), 33'333);

    FrameSampler sampler(samplingIntervalUs(AnalysisQuality::Standard));
    EXPECT_EQ(sampler.estimateFrameCount(10'000'000).value_or(0), 101);
}

// ------------------------------------------------------- provider abstraction

TEST(DetectionProviderRegistry, RegistersBuiltInProvidersOnce) {
    registerBuiltinDetectionProviders();
    registerBuiltinDetectionProviders();  // idempotent

    auto& registry = DetectionProviderRegistry::instance();
    EXPECT_TRUE(registry.contains(MockDetectionProvider::kProviderId));

    int mockEntries = 0;
    for (const auto& provider : registry.providers()) {
        if (provider.id == MockDetectionProvider::kProviderId) ++mockEntries;
        EXPECT_FALSE(provider.displayName.empty());
        EXPECT_NE(provider.factory, nullptr);
    }
    EXPECT_EQ(mockEntries, 1);

    // Creating does not load anything until initialise() is called.
    auto created = registry.create(MockDetectionProvider::kProviderId);
    ASSERT_NE(created, nullptr);
    EXPECT_FALSE(created->isReady());
    EXPECT_EQ(registry.create("no-such-provider"), nullptr);
}

TEST(MockProvider, IsDeterministicAndHonoursTheThreshold) {
    MockDetectionProvider provider;
    ASSERT_TRUE(provider.initialise(DetectionProviderConfig{}).ok());
    EXPECT_TRUE(provider.isReady());
    // It claims no model artefact, because it has none.
    EXPECT_FALSE(provider.info().modelSha256.has_value());

    const std::vector<std::uint8_t> pixels(16 * 16 * 3, 0);
    FrameInput frame;
    frame.rgb = pixels.data();
    frame.width = 16;
    frame.height = 16;
    frame.timestampUs = 4'000'000;  // an even second: person + car + dog

    DetectionOptions options;
    options.confidenceThreshold = 0.20;
    auto first = provider.analyze(frame, options);
    ASSERT_TRUE(first.ok()) << first.error().toString();
    ASSERT_EQ(first.value().detections.size(), 3u);
    EXPECT_EQ(first.value().timestampUs, frame.timestampUs);

    // Same input, same output — that is the whole point of this provider.
    auto second = provider.analyze(frame, options);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().detections.size(), first.value().detections.size());
    for (std::size_t i = 0; i < first.value().detections.size(); ++i) {
        EXPECT_EQ(first.value().detections[i].classLabel, second.value().detections[i].classLabel);
        EXPECT_DOUBLE_EQ(first.value().detections[i].confidence,
                         second.value().detections[i].confidence);
        EXPECT_DOUBLE_EQ(first.value().detections[i].box.x, second.value().detections[i].box.x);
    }

    // Raising the threshold removes the low-confidence object.
    options.confidenceThreshold = 0.50;
    auto filtered = provider.analyze(frame, options);
    ASSERT_TRUE(filtered.ok());
    EXPECT_EQ(filtered.value().detections.size(), 2u);
    for (const auto& detection : filtered.value().detections) {
        EXPECT_GE(detection.confidence, 0.50);
    }

    // An odd second carries no vehicle.
    frame.timestampUs = 5'000'000;
    auto odd = provider.analyze(frame, options);
    ASSERT_TRUE(odd.ok());
    ASSERT_EQ(odd.value().detections.size(), 1u);
    EXPECT_EQ(odd.value().detections.front().classLabel, "person");

    // Class filtering.
    options.classFilter = {"car"};
    frame.timestampUs = 4'000'000;
    auto onlyCars = provider.analyze(frame, options);
    ASSERT_TRUE(onlyCars.ok());
    ASSERT_EQ(onlyCars.value().detections.size(), 1u);
    EXPECT_EQ(onlyCars.value().detections.front().classGroup, DetectionClassGroup::Vehicle);
}

TEST(MockProvider, RefusesToRunBeforeInitialisation) {
    MockDetectionProvider provider;
    const std::vector<std::uint8_t> pixels(4 * 4 * 3, 0);
    FrameInput frame;
    frame.rgb = pixels.data();
    frame.width = 4;
    frame.height = 4;
    EXPECT_FALSE(provider.analyze(frame, DetectionOptions{}).ok());
}

}  // namespace
}  // namespace trace
