// TRACE Phase 3 acceptance test — audio.
//
// Drives the real application through opening an item with sound: the waveform is
// built as a derived asset and appears on the timeline, and the audio transport
// reports what it can actually do on this machine.
//
// A note on what this test does not prove. Audio is rendered through QAudioSink,
// and a machine with no audio device has no sink to render into. On such a machine
// — a headless build agent, a locked-down terminal — the assertions below check the
// path that is actually taken: the transport is disabled, the reason names the
// missing device rather than blaming the recording, and video plays normally
// because the reference clock declines to answer. Whether sound comes out of a real
// sound card, and whether it stays in step with the picture, is not established
// here and cannot be; see docs/AUDIO.md.
//
// Three workflows:
//   §1 opening an item with audio: waveform built, registered, drawn
//   §2 the audio transport says what this machine can do, and why
//   §3 playback is unaffected by the presence or absence of a device
#include <gtest/gtest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QSlider>
#include <QThread>
#include <QToolButton>

#include <functional>
#include <memory>

#include "core/security/file_hasher.h"
#include "media/audio/audio_clock.h"
#include "media/audio/waveform.h"
#include "tests/support/test_environment.h"
#include "ui/app/application_context.h"
#include "ui/app/main_window.h"
#include "ui/audio/audio_output.h"
#include "ui/common/theme.h"
#include "ui/evidence_browser/evidence_panel.h"
#include "ui/timeline/timeline_widget.h"
#include "ui/viewer/viewer_panel.h"

namespace trace {
namespace {

/// Signs in the way the application does, so the acceptance tests exercise the
/// real startup path rather than a permissive one that exists only for tests.
/// Since local accounts arrived, UserContext grants no authority until a
/// credential has been verified — a test that skipped this would be testing a
/// TRACE nobody runs.
bool signInForTest(trace::ui::ApplicationContext& context) {
    constexpr const char* kUser = "acceptance";
    constexpr const char* kPassword = "acceptance test password";
    auto needed = context.auth().needsFirstRunSetup();
    if (!needed) return false;
    if (needed.value()) {
        if (!context.auth().createFirstAdministrator(kUser, "Acceptance Operator", kPassword)) {
            return false;
        }
    }
    return context.auth().signIn(kUser, kPassword).ok();
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) return true;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

struct Workspace {
    std::unique_ptr<testing::TemporaryDirectory> dataRoot;
    std::unique_ptr<ui::ApplicationContext> context;
    std::unique_ptr<ui::MainWindow> window;
    Evidence evidence;
    std::string caseId;

    static std::unique_ptr<Workspace> open(const std::string& prefix) {
        auto workspace = std::make_unique<Workspace>();
        workspace->dataRoot = std::make_unique<testing::TemporaryDirectory>(prefix);

        const auto incoming = workspace->dataRoot->path() / "incoming";
        std::filesystem::create_directories(incoming);
        const auto source = incoming / "sample.mp4";
        std::filesystem::copy_file(testing::sampleVideoPath(), source);

        workspace->context = std::make_unique<ui::ApplicationContext>();
        if (!workspace->context->initialise(workspace->dataRoot->path())) return nullptr;
        if (!signInForTest(*workspace->context)) return nullptr;

        CaseDraft draft;
        draft.caseNumber = "CASE-0003";
        draft.title = "Audio acceptance";
        draft.investigator = "A. Analyst";
        auto created = workspace->context->cases().createCase(draft);
        if (!created) return nullptr;
        workspace->caseId = created.value().id;
        if (!workspace->context->openCase(QString::fromStdString(workspace->caseId))) return nullptr;

        IngestRequest request;
        request.caseId = workspace->caseId;
        request.sourcePath = source;
        auto ingested = workspace->context->evidence().ingest(request);
        if (!ingested) return nullptr;
        workspace->evidence = ingested.value().evidence;

        workspace->window = std::make_unique<ui::MainWindow>(workspace->context.get());
        workspace->window->show();
        if (!waitFor([&] { return workspace->window->isVisible(); }, 10000)) return nullptr;

        workspace->window->evidencePanel()->refresh();
        workspace->window->evidencePanel()->selectEvidence(
            QString::fromStdString(workspace->evidence.id));
        if (!waitFor([&] { return workspace->context->currentEvidence().has_value(); }, 10000)) {
            return nullptr;
        }
        return workspace;
    }

    ~Workspace() {
        if (window != nullptr) {
            window->close();
            QApplication::processEvents();
        }
    }
};

// ════════════════════════════════════════════════ §1 the waveform as an asset

TEST(AcceptancePhase3, TheWaveformIsBuiltAsADerivedAssetAndReachesTheTimeline) {
    auto workspace = Workspace::open("trace-phase3-accept");
    ASSERT_NE(workspace, nullptr);

    // Built off the GUI thread when the item opens, so it arrives a moment later.
    ASSERT_TRUE(waitFor(
        [&] {
            auto assets = workspace->context->derivedAssets().listForEvidence(
                workspace->evidence.id);
            if (!assets) return false;
            for (const auto& asset : assets.value()) {
                if (asset.type == DerivedAssetType::Waveform) return true;
            }
            return false;
        },
        60000))
        << "no waveform was registered for an item that has an audio track";

    auto assets = workspace->context->derivedAssets().listForEvidence(workspace->evidence.id);
    ASSERT_TRUE(assets.ok());
    DerivedAsset waveformAsset;
    for (const auto& asset : assets.value()) {
        if (asset.type == DerivedAssetType::Waveform) waveformAsset = asset;
    }

    // A derivation carries its provenance like any other: what made it, from what,
    // and with which libraries.
    EXPECT_EQ(waveformAsset.evidenceId, workspace->evidence.id);
    ASSERT_TRUE(waveformAsset.sha256.has_value());
    EXPECT_FALSE(waveformAsset.sha256->empty());

    auto operations =
        workspace->context->derivedAssets().operationsForEvidence(workspace->evidence.id);
    ASSERT_TRUE(operations.ok());
    const ProcessingOperation* generation = nullptr;
    for (const auto& operation : operations.value()) {
        if (operation.operationType == "waveform_generation") generation = &operation;
    }
    ASSERT_NE(generation, nullptr) << "the waveform was registered with no operation recording "
                                      "how it was produced";
    EXPECT_NE(generation->parametersJson.find("buckets"), std::string::npos);
    EXPECT_NE(generation->parametersJson.find("normalised"), std::string::npos)
        << "the stored parameters must record that the envelope is not normalised, or a "
           "reader cannot tell a quiet recording from a rescaled one";
    EXPECT_NE(generation->libraryVersionsJson.find("libavcodec"), std::string::npos)
        << "which decoder produced these samples is part of the provenance";

    // The file on disk is the file that was hashed.
    const auto stored = workspace->context->layout().resolve(waveformAsset.storageRelPath);
    ASSERT_TRUE(std::filesystem::exists(stored));
    auto digest = hashFile(stored);
    ASSERT_TRUE(digest.ok());
    EXPECT_EQ(digest.value(), waveformAsset.sha256);

    // And the original is untouched, as after every derivation.
    auto originalDigest =
        hashFile(workspace->context->layout().resolve(workspace->evidence.storageRelPath));
    ASSERT_TRUE(originalDigest.ok());
    EXPECT_EQ(originalDigest.value(), workspace->evidence.sha256);

    // It reaches the timeline as an envelope row rather than a row of markers.
    auto* timeline = workspace->window->timeline();
    ASSERT_NE(timeline, nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            for (const auto& track : timeline->tracks()) {
                if (track.isEnvelope()) return true;
            }
            return false;
        },
        30000))
        << "the waveform never appeared on the timeline";

    const ui::TimelineTrack* audio = nullptr;
    for (const auto& track : timeline->tracks()) {
        if (track.isEnvelope()) audio = &track;
    }
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->name, QStringLiteral("Audio"));
    EXPECT_FALSE(audio->envelopePeaks.empty());
    EXPECT_EQ(audio->envelopePeaks.size(), audio->envelopeRms.size());
    for (std::size_t i = 0; i < audio->envelopePeaks.size(); ++i) {
        ASSERT_GE(audio->envelopePeaks[i], 0.0F);
        ASSERT_LE(audio->envelopePeaks[i], 1.0F);
        ASSERT_LE(audio->envelopeRms[i], audio->envelopePeaks[i] + 1e-5F)
            << "average energy exceeded the peak that contains it, at bucket " << i;
    }
}

// ══════════════════════════════════ §2 the transport says what it can actually do

TEST(AcceptancePhase3, TheAudioTransportReportsWhatThisMachineCanDoAndWhy) {
    auto workspace = Workspace::open("trace-phase3-transport");
    ASSERT_NE(workspace, nullptr);

    auto* viewer = workspace->window->viewerPanel();
    ASSERT_NE(viewer, nullptr);
    ASSERT_TRUE(waitFor([&] { return viewer->playback()->isOpen(); }, 30000));

    const bool deviceHere = ui::AudioOutput::deviceAvailable();
    const bool usable = viewer->playback()->hasAudio();
    EXPECT_EQ(usable, deviceHere)
        << "the sample has an audio track, so whether it can be played comes down to "
           "whether this machine has an output device";

    auto* mute = viewer->muteButton();
    auto* volume = viewer->volumeSlider();
    ASSERT_NE(mute, nullptr);
    ASSERT_NE(volume, nullptr);
    EXPECT_EQ(mute->isEnabled(), usable);
    EXPECT_EQ(volume->isEnabled(), usable);

    if (!usable) {
        // The distinction that matters: an analyst must not read "no device on this
        // workstation" as "this recording is silent".
        const QString reason = viewer->playback()->audioUnavailableReason();
        EXPECT_FALSE(reason.isEmpty());
        EXPECT_NE(reason.indexOf(QStringLiteral("audio output device")), -1)
            << "the reason given was: " << reason.toStdString();
        EXPECT_EQ(reason.indexOf(QStringLiteral("no audio track")), -1)
            << "the item does have an audio track; saying otherwise would misdescribe "
               "the evidence. Reason given: "
            << reason.toStdString();
        EXPECT_NE(mute->toolTip().indexOf(QStringLiteral("audio output device")), -1);
    } else {
        // Volume and muting change what leaves the sound card, and nothing else.
        auto before = hashFile(
            workspace->context->layout().resolve(workspace->evidence.storageRelPath));
        ASSERT_TRUE(before.ok());
        volume->setValue(35);
        mute->setChecked(true);
        QApplication::processEvents();
        auto after = hashFile(
            workspace->context->layout().resolve(workspace->evidence.storageRelPath));
        ASSERT_TRUE(after.ok());
        EXPECT_EQ(before.value(), after.value());
    }
}

// ═══════════════════════════════ §3 video is unaffected by the audio device

TEST(AcceptancePhase3, VideoPlaysWhetherOrNotThereIsAnythingToPlayItThrough) {
    auto workspace = Workspace::open("trace-phase3-playback");
    ASSERT_NE(workspace, nullptr);

    auto* viewer = workspace->window->viewerPanel();
    ASSERT_NE(viewer, nullptr);
    ASSERT_TRUE(waitFor([&] { return viewer->playback()->isOpen(); }, 30000));
    ASSERT_TRUE(waitFor([&] { return viewer->duration() > 0; }, 15000));

    // With no device the reference clock declines to answer and the engine paces
    // frames against their own timestamps, exactly as it did before audio existed.
    viewer->togglePlayPause();
    ASSERT_TRUE(waitFor([&] { return viewer->position() > 300'000; }, 30000))
        << "playback did not advance";
    viewer->playback()->pause();
    ASSERT_TRUE(waitFor([&] { return viewer->playback()->state() == PlaybackState::Paused; },
                        15000));

    // Seeking, stepping and speed changes all reach audio as well as video; none of
    // them may leave playback wedged.
    viewer->seek(3'000'000);
    ASSERT_TRUE(waitFor([&] { return viewer->position() <= 3'000'000 &&
                                     viewer->position() > 2'900'000; }, 20000))
        << "seek landed at " << viewer->position();

    const qint64 beforeStep = viewer->position();
    viewer->stepForward();
    ASSERT_TRUE(waitFor([&] { return viewer->position() > beforeStep; }, 20000));

    // A speed at which audio is silenced, and back again.
    viewer->playback()->setSpeed(4.0);
    EXPECT_FALSE(audioPlaysAtSpeed(4.0));
    viewer->togglePlayPause();
    ASSERT_TRUE(waitFor([&] { return viewer->position() > beforeStep + 500'000; }, 30000))
        << "playback did not advance at 4x";
    viewer->playback()->setSpeed(1.0);
    QApplication::processEvents();
    viewer->playback()->pause();
    ASSERT_TRUE(waitFor([&] { return viewer->playback()->state() == PlaybackState::Paused; },
                        15000))
        << "returning to normal speed left playback wedged";
}

}  // namespace
}  // namespace trace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    trace::ui::applyTraceTheme(application);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
