// Phase 3 — audio decoding and resampling against the real sample file.
//
// The committed fixture carries AAC mono at 44.1 kHz, so decoding it to stereo at
// 48 kHz exercises format conversion, channel upmixing and rate conversion at once.
#include <gtest/gtest.h>

#include <cmath>

#include "core/security/file_hasher.h"
#include "media/audio/waveform.h"
#include "media/audio/waveform_service.h"
#include "media/ffmpeg/audio_decoder.h"
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
