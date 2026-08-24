// Phase 3 — audio decoding and resampling against the real sample file.
//
// The committed fixture carries AAC mono at 44.1 kHz, so decoding it to stereo at
// 48 kHz exercises format conversion, channel upmixing and rate conversion at once.
#include <gtest/gtest.h>

#include <cmath>

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

}  // namespace
}  // namespace trace
