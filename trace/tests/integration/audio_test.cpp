// Phase 3 — audio decoding and resampling against the real sample file.
//
// The committed fixture carries AAC mono at 44.1 kHz, so decoding it to stereo at
// 48 kHz exercises format conversion, channel upmixing and rate conversion at once.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "core/security/file_hasher.h"
#include "media/audio/audio_clock.h"
#include "media/audio/waveform.h"
#include "media/audio/waveform_service.h"
#include "media/ffmpeg/audio_decoder.h"
#include "media/playback/playback_controller.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

constexpr Microseconds kClipDurationUs = 8'000'000;

TEST(AudioDecoderTest, ReportsWhatTheContainerHoldsAndWhatItProduces) {
    auto opened = AudioDecoder::open(testing::sampleVideoPath(), 48000, 2);
    ASSERT_TRUE(opened.ok()) << opened.error().message();
    auto decoder = opened.take();
    const AudioStreamInfo& info = decoder->info();

    // What was recorded, reported unchanged.
    EXPECT_EQ(info.codecName, "aac");
    EXPECT_EQ(info.sourceSampleRate, 44100);
    EXPECT_EQ(info.sourceChannels, 1);
    EXPECT_FALSE(info.sourceSampleFormat.empty());
    EXPECT_GE(info.streamIndex, 0);

    // What the caller asked for, and will actually receive.
    EXPECT_EQ(info.sampleRate, 48000);
    EXPECT_EQ(info.channels, 2);
    EXPECT_NEAR(static_cast<double>(info.durationUs), static_cast<double>(kClipDurationUs),
                200'000.0);
}

TEST(AudioDecoderTest, DecodesTheWholeTrackWithTimestampsThatOnlyMoveForward) {
    auto opened = AudioDecoder::open(testing::sampleVideoPath(), 48000, 2);
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();

    std::size_t totalFrames = 0;
    int blocks = 0;
    Microseconds previous = -1;
    Microseconds lastEnd = 0;

    for (;;) {
        auto block = decoder->nextBlock();
        if (!block) {
            EXPECT_EQ(block.error().code(), ErrorCode::NotFound)
                << "decoding should end cleanly, not fail: " << block.error().message();
            break;
        }
        const AudioBlockData data = block.take();
        ASSERT_TRUE(data.valid());
        EXPECT_EQ(data.channels, 2);
        EXPECT_EQ(data.sampleRate, 48000);
        EXPECT_EQ(data.samples.size() % 2, 0u) << "interleaved stereo must be a whole number of frames";

        EXPECT_GT(data.presentationUs, previous) << "block timestamps must advance";
        previous = data.presentationUs;
        lastEnd = data.presentationUs + data.durationUs();

        totalFrames += data.sampleFrames();
        ++blocks;
        ASSERT_LT(blocks, 5000) << "far more blocks than an eight second clip should produce";
    }

    EXPECT_GT(blocks, 0);
    // Eight seconds at 48 kHz, allowing for the encoder's padding at each end.
    const double seconds = static_cast<double>(totalFrames) / 48000.0;
    EXPECT_NEAR(seconds, 8.0, 0.3) << "decoded " << totalFrames << " sample frames";
    EXPECT_NEAR(static_cast<double>(lastEnd), 8'000'000.0, 300'000.0);
}

TEST(AudioDecoderTest, TheFirstBlockStartsAtTheBeginningOfTheStream) {
    auto opened = AudioDecoder::open(testing::sampleVideoPath(), 48000, 2);
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();

    auto first = decoder->nextBlock();
    ASSERT_TRUE(first.ok());
    // Normalised so the stream starts at zero, matching how video frames are reported,
    // so an audio position and a video position mean the same thing.
    EXPECT_LT(first.value().presentationUs, 100'000)
        << "the first block should be at or near time zero, was "
        << first.value().presentationUs;
}

TEST(AudioDecoderTest, ResamplesToWhateverFormatIsAskedFor) {
    // The same source, decoded three ways.
    struct Target {
        int rate;
        int channels;
    };
    for (const Target target : {Target{48000, 2}, Target{44100, 1}, Target{16000, 1}}) {
        auto opened = AudioDecoder::open(testing::sampleVideoPath(), target.rate, target.channels);
        ASSERT_TRUE(opened.ok()) << target.rate << "/" << target.channels;
        auto decoder = opened.take();
        EXPECT_EQ(decoder->info().sampleRate, target.rate);
        EXPECT_EQ(decoder->info().channels, target.channels);

        std::size_t frames = 0;
        for (;;) {
            auto block = decoder->nextBlock();
            if (!block) break;
            const AudioBlockData data = block.take();
            EXPECT_EQ(data.channels, target.channels);
            EXPECT_EQ(data.sampleRate, target.rate);
            frames += data.sampleFrames();
        }
        const double seconds = static_cast<double>(frames) / target.rate;
        EXPECT_NEAR(seconds, 8.0, 0.3) << "at " << target.rate << " Hz";
    }
}

TEST(AudioDecoderTest, SeekingLandsWhereItSaysItDoes) {
    auto opened = AudioDecoder::open(testing::sampleVideoPath(), 48000, 2);
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();

    ASSERT_TRUE(decoder->seek(4'000'000).ok());
    auto block = decoder->nextBlock();
    ASSERT_TRUE(block.ok()) << block.error().message();

    // A seek lands on a packet boundary at or before the target; the timestamp reports
    // where the audio genuinely resumes rather than where it was asked to.
    EXPECT_LE(block.value().presentationUs, 4'000'000 + 100'000);
    EXPECT_GT(block.value().presentationUs, 3'000'000)
        << "seeking to four seconds should not land back near the beginning";

    // And decoding continues from there to the end.
    Microseconds last = block.value().presentationUs;
    int blocks = 1;
    for (;;) {
        auto next = decoder->nextBlock();
        if (!next) break;
        EXPECT_GT(next.value().presentationUs, last);
        last = next.value().presentationUs;
        ++blocks;
    }
    EXPECT_GT(blocks, 1);
    EXPECT_NEAR(static_cast<double>(last), 8'000'000.0, 300'000.0);
}

TEST(AudioDecoderTest, SeekingBackToZeroReplaysFromTheStart) {
    auto opened = AudioDecoder::open(testing::sampleVideoPath(), 48000, 2);
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();

    ASSERT_TRUE(decoder->seek(5'000'000).ok());
    ASSERT_TRUE(decoder->nextBlock().ok());
    ASSERT_TRUE(decoder->seek(0).ok());

    auto block = decoder->nextBlock();
    ASSERT_TRUE(block.ok());
    EXPECT_LT(block.value().presentationUs, 200'000)
        << "after seeking home the next block should be at the start";
}

TEST(AudioDecoderTest, AnItemWithNoAudioTrackSaysSoRatherThanFailingObscurely) {
    const auto clip = testing::pedestrianVideoPath();
    if (!clip) {
        GTEST_SKIP() << "the video-only clip is not present — run scripts/fetch_test_media.sh";
    }

    auto opened = AudioDecoder::open(*clip, 48000, 2);
    ASSERT_FALSE(opened.ok()) << "this clip has no audio stream";
    EXPECT_EQ(opened.error().code(), ErrorCode::NotFound);
    EXPECT_NE(opened.error().message().find("no audio track"), std::string::npos)
        << "the message should say what is absent: " << opened.error().message();
}

TEST(AudioDecoderTest, RefusesAnImpossibleOutputFormat) {
    EXPECT_FALSE(AudioDecoder::open(testing::sampleVideoPath(), 0, 2).ok());
    EXPECT_FALSE(AudioDecoder::open(testing::sampleVideoPath(), 48000, 0).ok());
    EXPECT_FALSE(AudioDecoder::open(testing::sampleVideoPath(), 48000, 64).ok());
}

TEST(AudioDecoderTest, DecodingDoesNotModifyTheSourceFile) {
    // The same rule as everywhere else: reading evidence never writes to it.
    testing::TemporaryDirectory scratch("trace-audio");
    const auto copy = scratch.path() / "sample.mp4";
    std::filesystem::copy_file(testing::sampleVideoPath(), copy);

    const auto before = std::filesystem::last_write_time(copy);
    const auto size = std::filesystem::file_size(copy);

    auto opened = AudioDecoder::open(copy, 48000, 2);
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();
    for (int i = 0; i < 20; ++i) {
        if (!decoder->nextBlock()) break;
    }
    decoder.reset();

    EXPECT_EQ(std::filesystem::file_size(copy), size);
    EXPECT_EQ(std::filesystem::last_write_time(copy), before);
}


// ══════════════════════════════════════════════════════════════ waveforms

// ---------------------------------------------------------------------------
// The reference clock. This is the arithmetic that decides where video is drawn
// relative to what is being heard, so it is tested on its own — an audio device
// is not needed to get it wrong.
// ---------------------------------------------------------------------------

TEST(AudioClockTest, SaysNothingUntilItIsRunning) {
    AudioClock clock;
    EXPECT_FALSE(clock.running());
    EXPECT_FALSE(clock.positionUs().has_value())
        << "a clock that is not running must not offer a position: the caller has to "
           "know to fall back to its own timing";

    clock.start(0);
    EXPECT_TRUE(clock.running());
    ASSERT_TRUE(clock.positionUs().has_value());

    clock.stop();
    EXPECT_FALSE(clock.positionUs().has_value())
        << "a stopped clock must not keep answering with a position that has quietly "
           "stopped advancing";
}

TEST(AudioClockTest, ReportsWhereTheDeviceIsOnTheMediaTimeline) {
    AudioClock clock;

    // Playback started five seconds in, as it would after a seek.
    clock.start(5'000'000);
    EXPECT_EQ(clock.positionUs().value(), 5'000'000)
        << "before the device has said anything the clock is exactly where it was told "
           "to start, not an extrapolation from the system clock";

    // The device counts from its own start, not from the media timeline: that
    // offset is what this class exists to add back.
    clock.reportDevicePosition(250'000);
    EXPECT_EQ(clock.positionUs().value(), 5'250'000);

    clock.reportDevicePosition(1'000'000);
    EXPECT_EQ(clock.positionUs().value(), 6'000'000);
}

TEST(AudioClockTest, ASeekMovesTheOriginRatherThanAccumulating) {
    AudioClock clock;
    clock.start(0);
    clock.reportDevicePosition(3'000'000);
    ASSERT_EQ(clock.positionUs().value(), 3'000'000);

    // Restarting is what a seek does. The device's count restarts with it, so the
    // three seconds already played must not be carried over.
    clock.start(30'000'000);
    EXPECT_EQ(clock.positionUs().value(), 30'000'000);
    clock.reportDevicePosition(100'000);
    EXPECT_EQ(clock.positionUs().value(), 30'100'000);
}

TEST(AudioClockTest, ADeviceCountThatJumpsBackwardsDoesNotDragPlaybackBack) {
    AudioClock clock;
    clock.start(0);
    clock.reportDevicePosition(2'000'000);
    clock.reportDevicePosition(1'000'000);  // some backends do this around an underrun
    EXPECT_EQ(clock.positionUs().value(), 2'000'000)
        << "video would visibly jump backwards; a real backwards move on the media "
           "timeline is a seek, and a seek restarts the clock";

    clock.reportDevicePosition(-5'000);
    EXPECT_EQ(clock.positionUs().value(), 2'000'000);
}

TEST(AudioClockTest, AudioIsRenderedOnlyAtNormalSpeed) {
    EXPECT_TRUE(audioPlaysAtSpeed(1.0));
    // Every other speed the viewer offers. Resampling to reach them would shift the
    // pitch of a recorded voice, which misrepresents the recording; TRACE silences
    // the track instead of altering it.
    for (double speed : kPlaybackSpeeds) {
        if (speed == 1.0) continue;
        EXPECT_FALSE(audioPlaysAtSpeed(speed)) << "speed " << speed;
    }
}

TEST(PlaybackClockSourceTest, VideoFollowsTheReferenceClockWhenThereIsOne) {
    PlaybackController controller;

    // A clock standing well ahead of the start: if video is really pacing against
    // it, the frames it hurries through are the ones before that position.
    std::atomic<Microseconds> reference{4'000'000};
    controller.setClockSource(
        [&reference]() -> std::optional<Microseconds> { return reference.load(); });

    std::atomic<int> frames{0};
    std::atomic<Microseconds> latest{-1};
    controller.setFrameHandler([&](std::shared_ptr<const VideoFrameData> frame) {
        if (!frame) return;
        latest.store(frame->presentationUs);
        frames.fetch_add(1);
    });

    controller.open(testing::sampleVideoPath());
    ASSERT_TRUE(controller.waitForState({PlaybackState::Ready, PlaybackState::Error}, 10'000));
    ASSERT_EQ(controller.state(), PlaybackState::Ready);

    const auto startedAt = std::chrono::steady_clock::now();
    controller.play();
    // Four seconds of media in well under four seconds of wall time is only
    // possible if the frames were paced against the clock source rather than
    // against their own timestamps.
    while (latest.load() < 3'000'000 &&
           std::chrono::steady_clock::now() - startedAt < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    controller.pause();

    EXPECT_GE(latest.load(), 3'000'000) << "playback never reached the reference position";
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 3'000)
        << "three seconds of media took three seconds of wall time, so the clock source "
           "was ignored and the steady clock was still pacing";
    EXPECT_GT(frames.load(), 1);
}

TEST(PlaybackClockSourceTest, WithoutAClockSourceFramesKeepTheirOwnPace) {
    PlaybackController controller;

    // The source is installed but declines to answer, which is what a file with no
    // audio track does. Pacing has to fall back to the frames' own timestamps.
    std::atomic<int> asked{0};
    controller.setClockSource([&asked]() -> std::optional<Microseconds> {
        asked.fetch_add(1);
        return std::nullopt;
    });

    std::atomic<Microseconds> latest{-1};
    controller.setFrameHandler([&](std::shared_ptr<const VideoFrameData> frame) {
        if (frame) latest.store(frame->presentationUs);
    });

    controller.open(testing::sampleVideoPath());
    ASSERT_TRUE(controller.waitForState({PlaybackState::Ready, PlaybackState::Error}, 10'000));
    ASSERT_EQ(controller.state(), PlaybackState::Ready);

    const auto startedAt = std::chrono::steady_clock::now();
    controller.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    controller.pause();
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;

    EXPECT_GT(asked.load(), 0) << "the clock source was never consulted";
    ASSERT_GE(latest.load(), 0) << "no frame was delivered at all";
    // Real-time pacing: the media position reached cannot have run far ahead of the
    // wall time spent reaching it.
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    EXPECT_LT(latest.load(), elapsedUs + 500'000)
        << "media ran ahead of wall time, so nothing was pacing playback";
}

TEST(WaveformTest, BuildsAnEnvelopeThatSpansTheWholeTrack) {
    auto built = WaveformBuilder::build(testing::sampleVideoPath(), 512);
    ASSERT_TRUE(built.ok()) << built.error().message();
    const Waveform waveform = built.take();

    ASSERT_TRUE(waveform.valid());
    EXPECT_EQ(waveform.buckets(), 512u);
    EXPECT_EQ(waveform.peaks.size(), waveform.rms.size());
    EXPECT_NEAR(static_cast<double>(waveform.durationUs), 8'000'000.0, 300'000.0);

    // The source facts travel with it, so a viewer can say what was recorded.
    EXPECT_EQ(waveform.sourceSampleRate, 44100);
    EXPECT_EQ(waveform.sourceChannels, 1);

    // Every value is a real amplitude in range, and RMS never exceeds the peak that
    // contains it — if it did, one of the two is being computed wrongly.
    int nonSilent = 0;
    for (std::size_t i = 0; i < waveform.buckets(); ++i) {
        EXPECT_GE(waveform.peaks[i], 0.0F);
        EXPECT_LE(waveform.peaks[i], 1.0F);
        EXPECT_GE(waveform.rms[i], 0.0F);
        EXPECT_LE(waveform.rms[i], waveform.peaks[i] + 1e-4F)
            << "bucket " << i << ": rms " << waveform.rms[i] << " exceeds peak "
            << waveform.peaks[i];
        if (waveform.peaks[i] > 0.001F) ++nonSilent;
    }
    EXPECT_GT(nonSilent, 256) << "most of this clip carries a tone; the envelope looks empty";
}

TEST(WaveformTest, BucketCountIsIndependentOfHowLongTheRecordingIs) {
    // The point of a fixed bucket count: a short clip and a long one both fit one row.
    for (const int buckets : {64, 256, 2000}) {
        auto built = WaveformBuilder::build(testing::sampleVideoPath(), buckets);
        ASSERT_TRUE(built.ok()) << buckets;
        EXPECT_EQ(built.value().buckets(), static_cast<std::size_t>(buckets));
        EXPECT_GT(built.value().bucketDurationUs(), 0);
    }
}

TEST(WaveformTest, RefusesABucketCountThatWouldBeUseless) {
    EXPECT_FALSE(WaveformBuilder::build(testing::sampleVideoPath(), 0).ok());
    EXPECT_FALSE(WaveformBuilder::build(testing::sampleVideoPath(), 4).ok());
    EXPECT_FALSE(WaveformBuilder::build(testing::sampleVideoPath(), 1'000'000).ok());
}

TEST(WaveformTest, CancellationStopsTheBuild) {
    auto built = WaveformBuilder::build(testing::sampleVideoPath(), 512,
                                        [](double) { return false; });
    ASSERT_FALSE(built.ok());
    EXPECT_EQ(built.error().code(), ErrorCode::Cancelled);
}

TEST(WaveformTest, SurvivesARoundTripThroughItsStoredForm) {
    auto built = WaveformBuilder::build(testing::sampleVideoPath(), 128);
    ASSERT_TRUE(built.ok());
    const Waveform original = built.take();

    auto reloaded = Waveform::fromJson(original.toJson());
    ASSERT_TRUE(reloaded.ok()) << reloaded.error().message();
    const Waveform copy = reloaded.take();

    EXPECT_EQ(copy.durationUs, original.durationUs);
    EXPECT_EQ(copy.sourceSampleRate, original.sourceSampleRate);
    EXPECT_EQ(copy.sourceChannels, original.sourceChannels);
    ASSERT_EQ(copy.buckets(), original.buckets());
    for (std::size_t i = 0; i < copy.buckets(); ++i) {
        EXPECT_NEAR(copy.peaks[i], original.peaks[i], 1e-4F) << "bucket " << i;
        EXPECT_NEAR(copy.rms[i], original.rms[i], 1e-4F) << "bucket " << i;
    }
}

TEST(WaveformTest, AMalformedWaveformFileIsRejected) {
    EXPECT_FALSE(Waveform::fromJson("").ok());
    EXPECT_FALSE(Waveform::fromJson("{}").ok());
    EXPECT_FALSE(Waveform::fromJson("{\"duration_us\": 1000}").ok());
}

TEST(WaveformTest, IsGeneratedOnceAndRegisteredWithItsProvenance) {
    testing::TemporaryDirectory dataRoot("trace-waveform");
    auto stack = testing::TestStack::create(dataRoot.path());

    const auto incoming = dataRoot.path() / "incoming";
    std::filesystem::create_directories(incoming);
    const auto source = incoming / "sample.mp4";
    std::filesystem::copy_file(testing::sampleVideoPath(), source);

    CaseDraft draft;
    draft.caseNumber = "CASE-0001";
    draft.title = "Waveform";
    const Case caseRecord = stack.cases->createCase(draft).value();

    IngestRequest request;
    request.caseId = caseRecord.id;
    request.sourcePath = source;
    const Evidence evidence = stack.evidence->ingest(request).value().evidence;

    WaveformService service(*stack.layout, stack.derivedAssets);
    auto first = service.ensureWaveform(caseRecord.id, caseRecord.caseNumber, evidence, 256);
    ASSERT_TRUE(first.ok()) << first.error().message();
    const DerivedAsset asset = first.take();

    EXPECT_EQ(asset.type, DerivedAssetType::Waveform);
    ASSERT_TRUE(asset.sha256.has_value());
    EXPECT_FALSE(asset.sha256->empty());
    EXPECT_TRUE(std::filesystem::exists(stack.layout->resolve(asset.storageRelPath)));

    // Asking again returns the same asset rather than rebuilding it.
    auto second = service.ensureWaveform(caseRecord.id, caseRecord.caseNumber, evidence, 256);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().id, asset.id);

    // And it reads back as the envelope that was written.
    auto loaded = service.load(asset);
    ASSERT_TRUE(loaded.ok()) << loaded.error().message();
    EXPECT_EQ(loaded.value().buckets(), 256u);
    EXPECT_TRUE(loaded.value().valid());

    // The managed original is untouched, as with every other derivation.
    auto digest = hashFile(stack.layout->resolve(evidence.storageRelPath));
    ASSERT_TRUE(digest.ok());
    EXPECT_EQ(digest.value(), evidence.sha256);
}

TEST(WaveformTest, AnItemWithNoAudioReportsNothingToDrawRatherThanFailing) {
    const auto clip = testing::pedestrianVideoPath();
    if (!clip) GTEST_SKIP() << "the video-only clip is not present";

    auto built = WaveformBuilder::build(*clip, 256);
    ASSERT_FALSE(built.ok());
    EXPECT_EQ(built.error().code(), ErrorCode::NotFound)
        << "callers distinguish 'no audio' from 'something broke' by this code";
}

}  // namespace
}  // namespace trace
