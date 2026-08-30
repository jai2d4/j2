// Hardware-accelerated decoding.
//
// Read this before reading the assertions: **the machine these tests were
// written on has no accelerator of any kind.** Every device probe here comes
// back unavailable, so what is genuinely exercised is the enumeration, the
// fallback, and the guarantee that asking for hardware on a machine without it
// changes nothing about the decoded frames.
//
// The accelerated path itself is not executed. These tests are written so that
// on a machine that does have a GPU they exercise it rather than skipping —
// which is the only way the code gets tested at all — but on this one they are
// verifying the negative. `docs/HARDWARE_DECODE.md` says the same thing without
// the euphemism.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "media/ffmpeg/hardware_decode.h"
#include "media/ffmpeg/video_decoder.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

TEST(HardwareDecode, EnumerationAnswersForThisMachineNotForTheBuild) {
    // FFmpeg builds contain nearly every accelerator, so a list drawn from the
    // build would offer devices this machine has not got. Each entry here was
    // probed by opening it.
    const auto& devices = hwaccel::devices();
    for (const auto& device : devices) {
        EXPECT_FALSE(device.name.empty());
        EXPECT_FALSE(device.displayName.empty());
        if (!device.available) {
            EXPECT_FALSE(device.unavailableReason.empty())
                << device.name << " is unavailable and does not say why";
        }
    }
    // Repeated calls are cached and must not change their mind between the
    // settings dialog and the decoder.
    EXPECT_EQ(&devices, &hwaccel::devices());
    EXPECT_EQ(hwaccel::anyAvailable(), !hwaccel::availableDevices().empty());
}

TEST(HardwareDecode, AvailableDevicesAreASubsetOfWhatWasProbed) {
    const auto available = hwaccel::availableDevices();
    for (const auto& device : available) {
        EXPECT_TRUE(device.available);
        const auto& all = hwaccel::devices();
        EXPECT_NE(std::find_if(all.begin(), all.end(),
                               [&device](const hwaccel::Device& candidate) {
                                   return candidate.name == device.name;
                               }),
                  all.end());
    }
}

TEST(HardwareDecode, AskingForAnAcceleratorNeverStopsAFilePlaying) {
    // The fallback that matters on a mixed fleet: an accelerator that is absent,
    // busy, or unsupported for this codec must cost nothing but a log line.
    auto opened = VideoDecoder::openAccelerated(testing::sampleVideoPath(), "cuda");
    ASSERT_TRUE(opened.ok()) << opened.error().toString();
    auto decoder = opened.take();

    auto frame = decoder->nextFrame();
    ASSERT_TRUE(frame.ok()) << frame.error().toString();
    EXPECT_TRUE(frame.take().valid());
}

TEST(HardwareDecode, AnAcceleratorThatDidNotDecodeIsNotReportedAsIfItHad) {
    // The whole provenance story rests on this: what is recorded has to be what
    // decoded, not what was requested.
    auto opened = VideoDecoder::openAccelerated(testing::sampleVideoPath(),
                                                "a-device-that-does-not-exist");
    ASSERT_TRUE(opened.ok());
    auto decoder = opened.take();

    EXPECT_FALSE(decoder->usingHardware());
    EXPECT_TRUE(decoder->info().hardwareDevice.empty())
        << "the decoder claims an accelerator that did not decode anything";
    EXPECT_TRUE(decoder->nextFrame().ok());
}

TEST(HardwareDecode, RequestingHardwareOnAMachineWithoutItDecodesIdenticalFrames) {
    // On this machine the accelerated open falls back, so this compares software
    // with software and proves the fallback path changed nothing. On a machine
    // with a GPU it becomes a real comparison, which is the point.
    auto plainDecoder = VideoDecoder::open(testing::sampleVideoPath());
    auto requestedDecoder = VideoDecoder::openAccelerated(testing::sampleVideoPath());
    ASSERT_TRUE(plainDecoder.ok() && requestedDecoder.ok());

    auto plain = plainDecoder.take();
    auto requested = requestedDecoder.take();
    if (requested->usingHardware()) {
        GTEST_SKIP() << "an accelerator took the file; "
                        "HardwareDecodeOnRealHardware covers that case";
    }

    int compared = 0;
    for (int index = 0; index < 8; ++index) {
        auto a = plain->nextFrame();
        auto b = requested->nextFrame();
        if (!a.ok() || !b.ok()) break;
        const auto first = a.take();
        const auto second = b.take();
        ASSERT_EQ(first.presentationUs, second.presentationUs);
        ASSERT_EQ(first.rgb, second.rgb) << "the fallback path changed the pixels at frame "
                                         << index;
        ++compared;
    }
    EXPECT_GE(compared, 3);
}

TEST(HardwareDecode, TheComparisonHarnessRefusesToPretendItComparedAnything) {
    // verifyMatchesSoftware exists so a deployment can check its own hardware.
    // On a machine with none it must say so rather than returning a clean
    // result, because "identical" from a comparison that never ran is the most
    // misleading answer available.
    auto outcome = hwaccel::verifyMatchesSoftware(testing::sampleVideoPath(), "cuda", 4);
    if (hwaccel::anyAvailable()) {
        // Somewhere with real hardware: either it compared, or it explained.
        if (outcome.ok()) {
            const auto result = outcome.take();
            EXPECT_GT(result.framesCompared, 0);
            EXPECT_GE(result.maximumChannelDifference, 0);
        }
        return;
    }
    ASSERT_FALSE(outcome.ok()) << "a comparison claimed a result with no accelerator present";
    EXPECT_EQ(outcome.error().code(), ErrorCode::Unsupported);
}

// Runs only where an accelerator exists. It is written to be skipped here and to
// do real work elsewhere, rather than left out because this machine cannot run
// it — a test that does not exist is not going to be written later.
TEST(HardwareDecodeOnRealHardware, DecodedFramesMatchSoftwareOrTheDifferenceIsReported) {
    const auto available = hwaccel::availableDevices();
    if (available.empty()) {
        GTEST_SKIP() << "this machine has no hardware decoder; nothing to compare";
    }

    for (const auto& device : available) {
        auto outcome = hwaccel::verifyMatchesSoftware(testing::sampleVideoPath(), device.name, 8);
        if (!outcome.ok()) {
            // A device that cannot take this codec is an ordinary answer.
            EXPECT_EQ(outcome.error().code(), ErrorCode::Unsupported) << device.name;
            continue;
        }
        const auto result = outcome.take();
        EXPECT_GT(result.framesCompared, 0) << device.name;

        // Not asserted as a pass condition: a conformant decoder is bit-exact,
        // but whether this one is, is a fact about the hardware rather than
        // about TRACE. The number is reported so a deployment can see it.
        if (!result.identical()) {
            ADD_FAILURE() << device.name << " decoded differently from software: max channel "
                          << "difference " << result.maximumChannelDifference << ", mean "
                          << result.meanChannelDifference
                          << ". Frames exported through this device would not match frames "
                          << "exported in software.";
        }
    }
}

}  // namespace
}  // namespace trace
