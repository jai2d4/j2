// Decoding, seeking, frame stepping and frame export against the real sample.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "core/security/file_hasher.h"
#include "media/ffmpeg/media_probe.h"
#include "media/ffmpeg/video_decoder.h"
#include "media/playback/playback_controller.h"
#include "media/thumbnails/frame_export_service.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

constexpr std::int64_t kFrameDurationUs = 40'000;  // the sample is 25 fps

class MediaTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory = std::make_unique<testing::TemporaryDirectory>("trace-media");
        sample = directory->path() / "sample.mp4";
        std::filesystem::copy_file(testing::sampleVideoPath(), sample);
    }

    std::unique_ptr<testing::TemporaryDirectory> directory;
    std::filesystem::path sample;
};

TEST_F(MediaTest, ProbeReportsWhatTheContainerActuallyContains) {
    FFmpegMetadataExtractor extractor;
    auto extracted = extractor.extract(sample, "evidence-id");
    ASSERT_TRUE(extracted.ok()) << extracted.error().toString();
    const MediaMetadata metadata = extracted.take();

    EXPECT_EQ(metadata.extractionStatus, MetadataExtractionStatus::Ok);
    EXPECT_NE(metadata.containerFormat.value_or("").find("mp4"), std::string::npos);
    EXPECT_EQ(metadata.videoCodec.value_or(""), "h264");
    EXPECT_EQ(metadata.width.value_or(0), 320);
    EXPECT_EQ(metadata.height.value_or(0), 240);
    EXPECT_EQ(metadata.pixelFormat.value_or(""), "yuv420p");
    EXPECT_EQ(metadata.displayAspect.value_or(""), "4:3");
    EXPECT_NEAR(metadata.averageFrameRate.value_or(0.0), 25.0, 0.01);
    EXPECT_EQ(metadata.frameRateMode, FrameRateMode::ConstantSuspected);
    EXPECT_EQ(metadata.audioCodec.value_or(""), "aac");
    EXPECT_EQ(metadata.audioSampleRate.value_or(0), 44100);
    EXPECT_EQ(metadata.streams.size(), 2u);
    EXPECT_FALSE(metadata.extractor.empty());
    EXPECT_NE(metadata.rawMetadataJson.find("\"streams\""), std::string::npos);

    // Nothing is invented for a container that carries no GPS or camera model.
    EXPECT_FALSE(metadata.gpsLocation.has_value());
    EXPECT_FALSE(metadata.make.has_value());
}

TEST_F(MediaTest, ProbeFailsCleanlyOnSomethingThatIsNotMedia) {
    const auto text = directory->path() / "statement.txt";
    { std::ofstream(text) << "not media"; }

    FFmpegMetadataExtractor extractor;
    auto extracted = extractor.extract(text, "evidence-id");
    ASSERT_FALSE(extracted.ok());
    EXPECT_EQ(extracted.error().code(), ErrorCode::MediaError);
    EXPECT_NE(extracted.error().message().find("unchanged"), std::string::npos)
        << "the operator should be told the stored evidence is unaffected";
}

TEST_F(MediaTest, DecoderReportsStreamFactsAndDecodesInOrder) {
    auto opened = VideoDecoder::open(sample);
    ASSERT_TRUE(opened.ok()) << opened.error().toString();
    auto decoder = opened.take();

    EXPECT_EQ(decoder->info().width, 320);
    EXPECT_EQ(decoder->info().height, 240);
    EXPECT_EQ(decoder->info().codecName, "h264");
    EXPECT_TRUE(decoder->info().hasAudio);
    EXPECT_NEAR(static_cast<double>(decoder->info().durationUs), 8'000'000.0, 200'000.0);
    EXPECT_EQ(decoder->estimatedFrameDurationUs(), kFrameDurationUs);

    std::int64_t previous = -1;
    for (int i = 0; i < 10; ++i) {
        auto frame = decoder->nextFrame();
        ASSERT_TRUE(frame.ok()) << frame.error().toString();
        EXPECT_EQ(frame.value().width, 320);
        EXPECT_EQ(frame.value().height, 240);
        EXPECT_EQ(frame.value().rgb.size(), 320u * 240u * 3u);
        EXPECT_GT(frame.value().presentationUs, previous);
        previous = frame.value().presentationUs;
    }
    EXPECT_NEAR(static_cast<double>(previous), 9 * kFrameDurationUs, 1000.0);
}

TEST_F(MediaTest, SeekingIsFrameAccurateNotKeyframeAccurate) {
    auto opened = VideoDecoder::open(sample);
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();

    for (const std::int64_t target : {0LL, 1'000'000LL, 5'000'000LL, 5'020'000LL, 7'000'000LL}) {
        auto frame = decoder->frameAt(target);
        ASSERT_TRUE(frame.ok()) << "seek to " << target << ": " << frame.error().toString();
        EXPECT_LE(frame.value().presentationUs, target)
            << "seek overshot the requested position";
        EXPECT_GT(frame.value().presentationUs, target - kFrameDurationUs)
            << "seek landed more than one frame early — that is keyframe seeking, not accurate "
               "seeking";
    }
}

TEST_F(MediaTest, FrameSteppingMovesExactlyOneFrameInEitherDirection) {
    auto opened = VideoDecoder::open(sample);
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();

    auto start = decoder->frameAt(4'000'000);
    ASSERT_TRUE(start.ok());
    const std::int64_t startPosition = start.value().presentationUs;

    auto forward = decoder->nextFrame();
    ASSERT_TRUE(forward.ok());
    EXPECT_EQ(forward.value().presentationUs - startPosition, kFrameDurationUs);

    auto back = decoder->frameBefore(forward.value().presentationUs);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value().presentationUs, startPosition)
        << "stepping back did not return to the previous frame";

    auto atZero = decoder->frameBefore(0);
    ASSERT_TRUE(atZero.ok());
    EXPECT_EQ(atZero.value().presentationUs, 0) << "stepping back from the first frame must hold";
}

TEST_F(MediaTest, DecodingRunsToTheEndAndStopsCleanly) {
    auto opened = VideoDecoder::open(sample);
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();

    int frames = 0;
    for (;;) {
        auto frame = decoder->nextFrame();
        if (!frame) {
            EXPECT_EQ(frame.error().code(), ErrorCode::NotFound);
            break;
        }
        ++frames;
        ASSERT_LT(frames, 1000) << "decoding did not terminate";
    }
    EXPECT_EQ(frames, 200) << "the sample contains 200 frames";
}

TEST_F(MediaTest, UnsupportedInputIsExplainedRatherThanCrashing) {
    const auto text = directory->path() / "statement.txt";
    { std::ofstream(text) << "not media"; }

    auto opened = VideoDecoder::open(text);
    ASSERT_FALSE(opened.ok());
    EXPECT_TRUE(opened.error().code() == ErrorCode::MediaError ||
                opened.error().code() == ErrorCode::Unsupported);
    EXPECT_FALSE(opened.error().message().empty());
}

TEST_F(MediaTest, PlaybackControllerOpensSeeksAndSteps) {
    PlaybackController controller;
    std::atomic<int> framesDelivered{0};
    controller.setFrameHandler([&](std::shared_ptr<const VideoFrameData> frame) {
        if (frame && frame->valid()) framesDelivered.fetch_add(1);
    });

    controller.open(sample);
    ASSERT_TRUE(controller.waitForState({PlaybackState::Ready, PlaybackState::Error}, 10000));
    ASSERT_EQ(controller.state(), PlaybackState::Ready) << controller.lastError();
    EXPECT_TRUE(controller.isOpen());
    EXPECT_NEAR(static_cast<double>(controller.duration()), 8'000'000.0, 200'000.0);
    EXPECT_GE(framesDelivered.load(), 1) << "opening should present the first frame";

    controller.seek(5'000'000);
    ASSERT_TRUE(controller.waitForIdleQueue(10000));
    EXPECT_LE(controller.position(), 5'000'000);
    EXPECT_GT(controller.position(), 5'000'000 - kFrameDurationUs);

    const std::int64_t afterSeek = controller.position();
    controller.stepForward();
    ASSERT_TRUE(controller.waitForIdleQueue(10000));
    EXPECT_EQ(controller.position() - afterSeek, kFrameDurationUs);

    controller.stepBackward();
    ASSERT_TRUE(controller.waitForIdleQueue(10000));
    EXPECT_EQ(controller.position(), afterSeek);

    auto current = controller.currentFrame();
    ASSERT_NE(current, nullptr);
    EXPECT_TRUE(current->valid());
}

TEST_F(MediaTest, PlaybackControllerPlaysAndPausesOnRequest) {
    PlaybackController controller;
    controller.open(sample);
    ASSERT_TRUE(controller.waitForState({PlaybackState::Ready, PlaybackState::Error}, 10000));
    ASSERT_EQ(controller.state(), PlaybackState::Ready);

    controller.setSpeed(4.0);
    controller.play();
    ASSERT_TRUE(controller.waitForState({PlaybackState::Playing}, 5000));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (controller.position() < 1'000'000 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    EXPECT_GE(controller.position(), 1'000'000) << "playback did not advance";

    controller.pause();
    ASSERT_TRUE(controller.waitForState({PlaybackState::Paused}, 5000));
    const std::int64_t paused = controller.position();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(controller.position(), paused) << "playback continued after pause";
}

TEST_F(MediaTest, FrameExportProducesADerivedAssetWithFullProvenance) {
    testing::TemporaryDirectory dataRoot("trace-export");
    auto stack = testing::TestStack::create(dataRoot.path());

    CaseDraft draft;
    draft.title = "Frame export";
    auto created = stack.cases->createCase(draft);
    ASSERT_TRUE(created.ok());
    const Case owner = created.take();

    IngestRequest request;
    request.caseId = owner.id;
    request.sourcePath = sample;
    auto imported = stack.evidence->ingest(request);
    ASSERT_TRUE(imported.ok()) << imported.error().toString();
    const Evidence evidence = imported.value().evidence;

    auto opened = VideoDecoder::open(stack.layout->resolve(evidence.storageRelPath));
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();
    auto frame = decoder->frameAt(5'000'000);
    ASSERT_TRUE(frame.ok());
    const VideoFrameData frameData = frame.take();

    FrameExportService exports(*stack.layout, stack.derivedAssets);
    FrameExportService::ExportRequest exportRequest;
    exportRequest.caseId = owner.id;
    exportRequest.caseNumber = owner.caseNumber;
    exportRequest.evidence = evidence;
    exportRequest.frame = &frameData;
    exportRequest.decoderInfo = decoder->info();

    auto exported = exports.exportFrame(exportRequest);
    ASSERT_TRUE(exported.ok()) << exported.error().toString();
    const DerivedAsset asset = exported.take();

    // The file exists, is a PNG, and is hashed.
    const auto path = stack.layout->resolve(asset.storageRelPath);
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_NE(asset.storageRelPath.find("exports"), std::string::npos);
    ASSERT_TRUE(asset.sha256.has_value());
    auto recomputed = hashFile(path);
    ASSERT_TRUE(recomputed.ok());
    EXPECT_EQ(recomputed.value(), *asset.sha256);

    std::ifstream png(path, std::ios::binary);
    char signature[8] = {};
    png.read(signature, 8);
    EXPECT_EQ(static_cast<unsigned char>(signature[0]), 0x89);
    EXPECT_EQ(signature[1], 'P');
    EXPECT_EQ(signature[2], 'N');
    EXPECT_EQ(signature[3], 'G');

    // The provenance chain is complete: source evidence, timestamp, operation,
    // parameters, software version, operator, and no self-certification.
    EXPECT_EQ(asset.evidenceId, evidence.id);
    EXPECT_EQ(asset.type, DerivedAssetType::FrameExport);
    EXPECT_EQ(asset.sourceStartUs.value_or(-1), frameData.presentationUs);
    EXPECT_EQ(asset.verificationState, VerificationState::Unverified);
    ASSERT_TRUE(asset.operationId.has_value());

    auto operations = stack.derivedAssets->operationsForEvidence(evidence.id);
    ASSERT_TRUE(operations.ok());
    ASSERT_FALSE(operations.value().empty());
    const ProcessingOperation& operation = operations.value().front();
    EXPECT_EQ(operation.operationType, "frame_extraction");
    EXPECT_EQ(operation.softwareName, "TRACE");
    EXPECT_FALSE(operation.softwareVersion.empty());
    EXPECT_NE(operation.libraryVersionsJson.find("libavcodec"), std::string::npos);
    EXPECT_NE(operation.parametersJson.find("source_timecode"), std::string::npos);
    EXPECT_EQ(operation.sourceStartUs.value_or(-1), frameData.presentationUs);
    EXPECT_FALSE(operation.performedBy.empty());

    // The original is untouched by the export.
    auto verified = stack.integrity->verify(evidence, owner.caseNumber);
    ASSERT_TRUE(verified.ok());
    EXPECT_TRUE(verified.value().verified);

    AuditQuery query;
    query.action = AuditAction::FrameExtracted;
    auto events = stack.audit->list(query);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1u);
    EXPECT_NE(events.value().front().detailsJson.find("frame_extraction"), std::string::npos);
    EXPECT_NE(events.value().front().detailsJson.find("00:00:05"), std::string::npos);
}

TEST_F(MediaTest, ThumbnailGenerationIsIdempotentAndRecorded) {
    testing::TemporaryDirectory dataRoot("trace-thumb");
    auto stack = testing::TestStack::create(dataRoot.path());

    CaseDraft draft;
    draft.title = "Thumbnails";
    auto created = stack.cases->createCase(draft);
    ASSERT_TRUE(created.ok());
    const Case owner = created.take();

    IngestRequest request;
    request.caseId = owner.id;
    request.sourcePath = sample;
    auto imported = stack.evidence->ingest(request);
    ASSERT_TRUE(imported.ok());

    FrameExportService exports(*stack.layout, stack.derivedAssets);
    auto first = exports.ensureThumbnail(owner.id, owner.caseNumber, imported.value().evidence);
    ASSERT_TRUE(first.ok()) << first.error().toString();
    auto second = exports.ensureThumbnail(owner.id, owner.caseNumber, imported.value().evidence);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value().id, second.value().id) << "a second call must reuse the preview";

    EXPECT_TRUE(std::filesystem::exists(stack.layout->resolve(first.value().storageRelPath)));
    EXPECT_NE(first.value().storageRelPath.find("thumbnails"), std::string::npos);
    EXPECT_EQ(first.value().type, DerivedAssetType::Thumbnail);
}

}  // namespace
}  // namespace trace
