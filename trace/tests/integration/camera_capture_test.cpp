// Connecting TRACE to a camera, and recording from one.
//
// Read this before the assertions: **the machine these tests were written on
// has no camera of any kind** — no USB device under /dev/video*, no Bluetooth
// adapter, and no IP camera on the network. What is genuinely exercised is the
// transport model, the discovery paths as far as this machine allows, the
// refusals, and — over a real TCP socket served by the test itself — the live
// capture path end to end: open, remux, close, hash, register, and a link that
// drops mid-recording.
//
// What is not exercised: the RTSP handshake (no server available here), USB and
// capture-card capture (no device), and Bluetooth control (no adapter, and no
// backend written against one). docs/CAMERA_INGEST.md states the same without
// the euphemism.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "core/security/file_hasher.h"
#include "media/capture/bluetooth.h"
#include "media/capture/camera.h"
#include "media/capture/capture_service.h"
#include "media/capture/capture_session.h"
#include "media/capture/discovery.h"
#include "media/ffmpeg/video_decoder.h"
#include "tests/support/stream_server.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

using testing::StreamServer;
using testing::TemporaryDirectory;

// ---------------------------------------------------------------- the model

TEST(CameraModel, BluetoothIsTheOnlyTransportThatCannotCarryVideo) {
    EXPECT_TRUE(carriesVideo(CameraTransport::NetworkStream));
    EXPECT_TRUE(carriesVideo(CameraTransport::LocalDevice));
    EXPECT_FALSE(carriesVideo(CameraTransport::BluetoothControl));
}

TEST(CameraModel, EthernetAndWifiAreTheSameTransport) {
    // The request that produced this feature named cable and WiFi separately.
    // Above the network stack they are one path, and the model must not grow a
    // second implementation to match the wording of the request.
    auto wired = cameraSourceFromUri("rtsp://192.0.2.10:554/stream1");
    auto wireless = cameraSourceFromUri("rtsp://192.0.2.11:554/stream1");
    ASSERT_TRUE(wired.ok()) << wired.error().toString();
    ASSERT_TRUE(wireless.ok()) << wireless.error().toString();
    EXPECT_EQ(wired.value().transport, CameraTransport::NetworkStream);
    EXPECT_EQ(wireless.value().transport, CameraTransport::NetworkStream);
}

TEST(CameraModel, EveryTransportTraceActuallySupportsIsAccepted) {
    for (const char* uri : {"rtsp://192.0.2.10/s", "rtsps://192.0.2.10/s", "rtmp://192.0.2.10/s",
                            "rtmps://192.0.2.10/s", "srt://192.0.2.10:9000",
                            "http://192.0.2.10/video.mjpg", "https://192.0.2.10/video.mjpg",
                            "rtp://192.0.2.10:5000"}) {
        auto parsed = cameraSourceFromUri(uri);
        EXPECT_TRUE(parsed.ok()) << uri << ": " << (parsed.ok() ? "" : parsed.error().toString());
        if (parsed.ok()) {
            EXPECT_EQ(parsed.value().transport, CameraTransport::NetworkStream);
        }
    }
}

TEST(CameraModel, AnUnsupportedSchemeIsRefusedWhereItWasTypedNotInsideFfmpeg) {
    // The failure must name the problem. Passing this to avformat produces
    // "Invalid data found when processing input", which tells an operator
    // nothing about what they got wrong. The address is well formed — TRACE
    // simply does not reach cameras that way — so it is Unsupported rather than
    // InvalidArgument, and the detail lists what would work.
    auto parsed = cameraSourceFromUri("ftp://192.0.2.10/stream");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.error().code(), ErrorCode::Unsupported);
    EXPECT_FALSE(parsed.error().message().empty());
    EXPECT_NE(parsed.error().detail().find("rtsp://"), std::string::npos)
        << "the refusal does not tell the operator what to type instead";
}

TEST(CameraModel, AnEmptyAddressIsRefused) {
    EXPECT_FALSE(cameraSourceFromUri("").ok());
    EXPECT_FALSE(cameraSourceFromUri("   ").ok());
}

TEST(CameraModel, CredentialsNeverAppearInAnythingLoggableOrDisplayable) {
    auto parsed = cameraSourceFromUri("rtsp://operator:hunter2@192.0.2.10:554/stream1");
    ASSERT_TRUE(parsed.ok()) << parsed.error().toString();
    const CameraSource camera = parsed.take();

    EXPECT_TRUE(camera.hasEmbeddedCredentials);
    // The URI keeps the password, because that is what has to be handed to
    // FFmpeg to open the camera at all.
    EXPECT_NE(camera.uri.find("hunter2"), std::string::npos);

    // Nothing that gets shown, logged or serialised may.
    EXPECT_EQ(camera.redactedUri().find("hunter2"), std::string::npos);
    EXPECT_EQ(camera.redactedUri().find("operator"), std::string::npos);
    EXPECT_EQ(camera.address.find("hunter2"), std::string::npos);
    EXPECT_EQ(camera.toJson().dump().find("hunter2"), std::string::npos);
    EXPECT_EQ(camera.id.find("hunter2"), std::string::npos);
}

TEST(CameraModel, RedactionLeavesAnAddressWithNoCredentialsAlone) {
    const std::string plain = "rtsp://192.0.2.10:554/stream1";
    EXPECT_EQ(redactCredentials(plain), plain);
    // An @ after the path is part of the path, not a credential separator.
    const std::string atInPath = "rtsp://192.0.2.10:554/live@main";
    EXPECT_EQ(redactCredentials(atInPath), atInPath);
}

TEST(CameraModel, TheSameCameraKeepsItsIdentityAcrossARescan) {
    auto first = cameraSourceFromUri("rtsp://192.0.2.10:554/stream1");
    auto second = cameraSourceFromUri("rtsp://192.0.2.10:554/stream1");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value().id, second.value().id);
    EXPECT_FALSE(first.value().id.empty());

    auto other = cameraSourceFromUri("rtsp://192.0.2.11:554/stream1");
    ASSERT_TRUE(other.ok());
    EXPECT_NE(first.value().id, other.value().id);
}

TEST(CameraModel, ManufacturerAndModelAreNeverGuessedFromTheAddress) {
    auto parsed = cameraSourceFromUri("rtsp://axis-camera.example:554/axis-media/media.amp");
    ASSERT_TRUE(parsed.ok());
    // "axis" is right there in the hostname and it is still not a claim TRACE
    // is entitled to make: a report that named the manufacturer would be
    // stating a fact nobody established.
    EXPECT_TRUE(parsed.value().manufacturer.empty());
    EXPECT_TRUE(parsed.value().model.empty());
    EXPECT_TRUE(parsed.value().reportedProfiles.empty());
}

TEST(CameraModel, ALinkThatWasNotDeterminedStaysUnknown) {
    auto parsed = cameraSourceFromUri("rtsp://192.0.2.10:554/stream1");
    ASSERT_TRUE(parsed.ok());
    // Guessing "wired" because it is an IP camera would put a wrong claim about
    // interruptibility into the provenance of every recording from it.
    EXPECT_EQ(parsed.value().link, CameraLink::Unknown);
}

// ------------------------------------------------------------------ discovery

TEST(CameraDiscovery, EnumerationDistinguishesNothingFoundFromCouldNotLook) {
    discovery::Options options;
    options.networkTimeoutMs = 300;
    auto outcome = discovery::findCameras(options);
    ASSERT_TRUE(outcome.ok()) << outcome.error().toString();

    // Whatever this machine has, a transport that could not be searched says so
    // with a reason. An operator staring at an empty list needs to know whether
    // to look for a camera or for a network.
    for (const auto& [transport, reason] : outcome.value().unavailable) {
        EXPECT_FALSE(transport.empty());
        EXPECT_FALSE(reason.empty()) << transport << " is unavailable and does not say why";
    }
    for (const auto& camera : outcome.value().cameras) {
        EXPECT_TRUE(camera.valid());
        EXPECT_FALSE(camera.id.empty());
    }
}

TEST(CameraDiscovery, LocalDeviceEnumerationIsCandidatesNotConfirmedCameras) {
    auto devices = discovery::findLocalDevices();
#if defined(_WIN32)
    // Said plainly rather than returned as an empty list that reads as "no
    // cameras attached".
    ASSERT_FALSE(devices.ok());
    EXPECT_EQ(devices.error().code(), ErrorCode::Unsupported);
    EXPECT_FALSE(devices.error().message().empty());
#else
    ASSERT_TRUE(devices.ok()) << devices.error().toString();
    for (const auto& device : devices.value()) {
        EXPECT_EQ(device.transport, CameraTransport::LocalDevice);
        EXPECT_FALSE(device.uri.empty());
    }
#endif
}

TEST(CameraDiscovery, NetworkDiscoveryEitherAnswersOrSaysWhyItCouldNot) {
    auto found = discovery::findNetworkCameras(300);
    if (!found) {
        EXPECT_FALSE(found.error().message().empty());
        EXPECT_FALSE(found.error().detail().empty());
        return;
    }
    for (const auto& camera : found.value()) {
        EXPECT_EQ(camera.transport, CameraTransport::NetworkStream);
        EXPECT_TRUE(camera.valid());
    }
}

TEST(CameraDiscovery, DisablingATransportDoesNotReportItAsUnavailable) {
    discovery::Options options;
    options.includeNetworkCameras = false;
    options.includeLocalDevices = false;
    auto outcome = discovery::findCameras(options);
    ASSERT_TRUE(outcome.ok());
    EXPECT_TRUE(outcome.value().cameras.empty());
    // Not searching is not the same as having tried and failed.
    EXPECT_TRUE(outcome.value().unavailable.empty());
}

// ------------------------------------------------------------------ bluetooth

TEST(CameraBluetooth, TheAnswerIsARefusalWithAReasonNotAnEmptyList) {
    auto scanned = bluetooth::scan(50);
    ASSERT_FALSE(scanned.ok())
        << "scan() returned a list; on a machine with no backend that reads as 'no cameras nearby'";
    EXPECT_EQ(scanned.error().code(), ErrorCode::Unsupported);
    EXPECT_FALSE(scanned.error().detail().empty());
}

TEST(CameraBluetooth, AvailabilityKeepsTheBackendAndTheAdapterApart) {
    const auto state = bluetooth::availability();
    // Two facts, because they need different things from an operator: install a
    // build with a backend, or plug in an adapter.
    EXPECT_FALSE(state.detail.empty());
    EXPECT_EQ(state.usable(), state.backendCompiled && state.adapterPresent);
    if (!state.usable()) {
        EXPECT_FALSE(state.detail.empty());
    }
}

TEST(CameraBluetooth, ConnectingNamesTheCameraItRefusedToConnectTo) {
    bluetooth::AdvertisedCamera camera;
    camera.address = "AA:BB:CC:DD:EE:FF";
    camera.name = "HERO12 Black";
    auto link = bluetooth::connect(camera);
    ASSERT_FALSE(link.ok());
    EXPECT_EQ(link.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(link.error().detail().find(camera.address), std::string::npos);
}

TEST(CameraBluetooth, TheExplanationSaysWhatBluetoothIsForAndWhatItIsNot) {
    const std::string explanation = bluetooth::videoOverBluetoothExplanation();
    ASSERT_FALSE(explanation.empty());
    // The sentence exists so the interface, the logs and the documentation
    // cannot drift; the point of it is naming the alternative that does work.
    EXPECT_NE(explanation.find("WiFi"), std::string::npos);
}

TEST(CameraBluetooth, EveryCommandHasAName) {
    for (const auto command :
         {bluetooth::Command::WakeUp, bluetooth::Command::StartRecording,
          bluetooth::Command::StopRecording, bluetooth::Command::EnableWifi,
          bluetooth::Command::DisableWifi, bluetooth::Command::ReadStatus}) {
        const std::string name = bluetooth::toString(command);
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name, "unknown");
    }
}

// -------------------------------------------------------------------- probing

TEST(CameraProbe, ABluetoothLinkIsRefusedWithoutTouchingTheNetwork) {
    CameraSource camera;
    camera.id = "cam-bt";
    camera.name = "HERO12 Black";
    camera.transport = CameraTransport::BluetoothControl;
    camera.link = CameraLink::Bluetooth;
    camera.address = "AA:BB:CC:DD:EE:FF";

    const auto start = std::chrono::steady_clock::now();
    const Status status = probeCamera(camera, 5);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error().code(), ErrorCode::Unsupported);
    // Refused on the transport, not after a five-second socket timeout.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
}

TEST(CameraProbe, ARealStreamAnswersAndAClosedPortDoesNot) {
    StreamServer server;
    StreamServer::Options options;
    options.chunkDelayMs = 2;
    ASSERT_TRUE(server.start(testing::sampleVideoPath(), options))
        << "could not open a loopback socket";

    auto camera = cameraSourceFromUri(server.url(), "test stream");
    ASSERT_TRUE(camera.ok()) << camera.error().toString();

    const Status reachable = probeCamera(camera.value(), 5);
    EXPECT_TRUE(reachable.ok()) << reachable.error().toString();
    server.stop();

    // The same address with nothing behind it. This must fail, and the failure
    // must be the one an operator can act on.
    const Status gone = probeCamera(camera.value(), 2);
    EXPECT_FALSE(gone.ok());
}

// ------------------------------------------------------------- live capture

TEST(CameraCapture, RecordsARealNetworkStreamToAFileThatDecodes) {
    TemporaryDirectory scratch("trace-capture");
    StreamServer server;
    StreamServer::Options options;
    options.chunkDelayMs = 3;
    ASSERT_TRUE(server.start(testing::sampleVideoPath(), options));

    auto camera = cameraSourceFromUri(server.url(), "test stream");
    ASSERT_TRUE(camera.ok()) << camera.error().toString();

    CaptureSession session;
    CaptureSettings settings;
    settings.maximumDurationMs = 20'000;  // a ceiling, not the expected length
    settings.openTimeoutSeconds = 5;
    // The end of this stream is a camera going away, and TRACE is right to
    // retry it. Shortened here so the test spends its time capturing rather
    // than waiting out a reconnect window whose behaviour has its own test.
    settings.reconnectWindowMs = 500;

    auto recorded = session.record(camera.take(), scratch.path(), "cam", settings);
    ASSERT_TRUE(recorded.ok()) << recorded.error().toString();
    const CaptureOutcome outcome = recorded.take();

    ASSERT_FALSE(outcome.segments.empty()) << "the capture produced no segments";
    const CaptureSegment& segment = outcome.segments.front();

    EXPECT_TRUE(segment.complete);
    EXPECT_GT(segment.bytesWritten, 0);
    EXPECT_GT(segment.framesWritten, 0);
    EXPECT_GT(segment.durationUs, 0);
    EXPECT_FALSE(segment.startedAt.empty());
    EXPECT_FALSE(segment.endedAt.empty());
    ASSERT_TRUE(std::filesystem::exists(segment.path));

    // The digest is of the file on disk, computed after it was closed.
    ASSERT_FALSE(segment.sha256.empty());
    auto rehashed = hashFile(segment.path);
    ASSERT_TRUE(rehashed.ok()) << rehashed.error().toString();
    EXPECT_EQ(rehashed.take(), segment.sha256);

    // A recording nothing can open is not evidence. This is the assertion that
    // makes the rest of the capture path meaningful.
    auto opened = VideoDecoder::open(segment.path);
    ASSERT_TRUE(opened.ok()) << "the captured file does not decode: " << opened.error().toString();
    EXPECT_GT(opened.value()->info().width, 0);
    EXPECT_GT(opened.value()->info().height, 0);

    auto frame = opened.value()->nextFrame();
    ASSERT_TRUE(frame.ok()) << frame.error().toString();
    EXPECT_TRUE(frame.value().valid());
}

TEST(CameraCapture, TheRecordingIsTheCamerasOwnBitstreamNotAReencode) {
    TemporaryDirectory scratch("trace-capture-codec");
    StreamServer server;
    StreamServer::Options options;
    options.chunkDelayMs = 2;
    ASSERT_TRUE(server.start(testing::sampleVideoPath(), options));

    auto camera = cameraSourceFromUri(server.url());
    ASSERT_TRUE(camera.ok());

    CaptureSession session;
    CaptureSettings settings;
    settings.maximumDurationMs = 20'000;
    settings.reconnectWindowMs = 500;
    settings.openTimeoutSeconds = 5;
    auto recorded = session.record(camera.take(), scratch.path(), "cam", settings);
    ASSERT_TRUE(recorded.ok()) << recorded.error().toString();
    ASSERT_FALSE(recorded.value().segments.empty());

    auto source = VideoDecoder::open(testing::sampleVideoPath());
    ASSERT_TRUE(source.ok()) << source.error().toString();
    auto captured = VideoDecoder::open(recorded.value().segments.front().path);
    ASSERT_TRUE(captured.ok()) << captured.error().toString();

    // Same codec and same dimensions: the packets were copied, not decoded and
    // encoded again. A re-encode would make every frame of the recording a
    // lossy derivative of the only copy that exists.
    EXPECT_EQ(captured.value()->info().codecName, source.value()->info().codecName);
    EXPECT_EQ(captured.value()->info().width, source.value()->info().width);
    EXPECT_EQ(captured.value()->info().height, source.value()->info().height);
}

TEST(CameraCapture, StoppingKeepsWhatWasRecordedAndSaysAnOperatorDidIt) {
    TemporaryDirectory scratch("trace-capture-stop");
    StreamServer server;
    StreamServer::Options options;
    // Small chunks, paced: the fixture takes several seconds to arrive this way,
    // so the stop lands while packets are still coming rather than after the
    // stream has already ended.
    options.chunkBytes = 1024;
    options.chunkDelayMs = 20;
    ASSERT_TRUE(server.start(testing::sampleVideoPath(), options));

    auto camera = cameraSourceFromUri(server.url());
    ASSERT_TRUE(camera.ok());

    CaptureSession session;
    std::thread stopper([&session] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        session.requestStop();
    });

    CaptureSettings settings;
    settings.maximumDurationMs = 30'000;
    // If the stop is honoured, none of this is reached. It is set low so that a
    // regression shows up as a failing assertion rather than a hung suite.
    settings.reconnectWindowMs = 500;
    settings.openTimeoutSeconds = 2;
    auto recorded = session.record(camera.take(), scratch.path(), "cam", settings);
    stopper.join();

    ASSERT_TRUE(recorded.ok()) << recorded.error().toString();
    const CaptureOutcome outcome = recorded.take();
    EXPECT_TRUE(outcome.stoppedByOperator);
    ASSERT_FALSE(outcome.segments.empty()) << "stopping discarded the recording";
    EXPECT_TRUE(outcome.segments.front().complete);
    // Closed properly and hashed, not abandoned half-written.
    EXPECT_FALSE(outcome.segments.front().sha256.empty());
    EXPECT_TRUE(VideoDecoder::open(outcome.segments.front().path).ok());
}

TEST(CameraCapture, ReturningFalseFromProgressStopsTheCapture) {
    TemporaryDirectory scratch("trace-capture-progress");
    StreamServer server;
    StreamServer::Options options;
    options.chunkBytes = 1024;
    options.chunkDelayMs = 15;
    ASSERT_TRUE(server.start(testing::sampleVideoPath(), options));

    auto camera = cameraSourceFromUri(server.url());
    ASSERT_TRUE(camera.ok());

    int calls = 0;
    CaptureSession session;
    CaptureSettings settings;
    settings.maximumDurationMs = 30'000;
    settings.reconnectWindowMs = 500;
    settings.openTimeoutSeconds = 2;
    auto recorded = session.record(camera.take(), scratch.path(), "cam", settings,
                                   [&calls](const CaptureProgress& state) {
                                       EXPECT_TRUE(state.connected);
                                       EXPECT_GE(state.bytesWritten, 0);
                                       return ++calls < 5;
                                   });
    ASSERT_TRUE(recorded.ok()) << recorded.error().toString();
    EXPECT_TRUE(recorded.value().stoppedByOperator);
    EXPECT_EQ(calls, 5);
}

TEST(CameraCapture, ADroppedLinkBecomesARecordedGapNotASilentJoin) {
    TemporaryDirectory scratch("trace-capture-gap");
    StreamServer server;
    StreamServer::Options options;
    options.chunkDelayMs = 3;
    options.dropAfterBytes = 40 * 1024;   // hang up mid-stream
    options.serveAgainAfterDrop = true;   // and come back
    ASSERT_TRUE(server.start(testing::sampleVideoPath(), options));

    auto camera = cameraSourceFromUri(server.url());
    ASSERT_TRUE(camera.ok());

    CaptureSession session;
    CaptureSettings settings;
    settings.maximumDurationMs = 30'000;
    settings.reconnectWindowMs = 8'000;
    settings.reconnectDelayMs = 100;
    settings.openTimeoutSeconds = 3;

    auto recorded = session.record(camera.take(), scratch.path(), "cam", settings);
    ASSERT_TRUE(recorded.ok()) << recorded.error().toString();
    const CaptureOutcome outcome = recorded.take();

    ASSERT_GE(server.connections(), 2) << "the capture never reconnected";
    ASSERT_FALSE(outcome.segments.empty());

    // The whole point: the capture is not continuous and says so.
    EXPECT_FALSE(outcome.continuous());
    ASSERT_FALSE(outcome.gaps.empty()) << "the link dropped and no gap was recorded";
    for (const auto& gap : outcome.gaps) {
        EXPECT_FALSE(gap.reason.empty()) << "a gap was recorded without a cause";
        EXPECT_GE(gap.atUs, 0);
    }

    // Separate files, not one file that presents continuous timestamps across
    // material that is missing a stretch of time.
    EXPECT_GE(outcome.segments.size(), 2u);
    for (const auto& segment : outcome.segments) {
        EXPECT_TRUE(segment.complete);
        EXPECT_FALSE(segment.sha256.empty());
        EXPECT_TRUE(VideoDecoder::open(segment.path).ok())
            << segment.path.string() << " does not decode";
    }

    // Wall clock covers the gap; recorded duration does not. An operator has to
    // be able to see both numbers or the missing time is invisible.
    EXPECT_GT(outcome.wallClockMs, 0);
}

TEST(CameraCapture, SplittingAtASizeLimitIsNotAGapAndDoesNotDropTheCamera) {
    TemporaryDirectory scratch("trace-capture-split");
    StreamServer server;
    StreamServer::Options options;
    options.chunkDelayMs = 2;
    ASSERT_TRUE(server.start(testing::sampleVideoPath(), options));

    auto camera = cameraSourceFromUri(server.url());
    ASSERT_TRUE(camera.ok());

    CaptureSession session;
    CaptureSettings settings;
    settings.maximumDurationMs = 20'000;
    settings.reconnectWindowMs = 500;
    settings.openTimeoutSeconds = 5;
    settings.segmentBytes = 48 * 1024;  // the fixture is ~160 KB, so several files

    auto recorded = session.record(camera.take(), scratch.path(), "cam", settings);
    ASSERT_TRUE(recorded.ok()) << recorded.error().toString();
    const CaptureOutcome outcome = recorded.take();

    ASSERT_GE(outcome.segments.size(), 2u) << "the size limit did not roll to a new file";

    // The two assertions this test exists for. A file boundary is not a
    // recording boundary: rolling must not record a gap that did not happen,
    // and — the reason it would have — must not close and reopen the camera,
    // which would lose whatever arrived in between and cause a real one.
    EXPECT_TRUE(outcome.continuous())
        << "splitting at a size limit was recorded as an interruption";
    EXPECT_EQ(server.connections(), 1)
        << "the camera was dropped and reconnected just to start a new file";

    for (const auto& segment : outcome.segments) {
        EXPECT_TRUE(segment.complete);
        EXPECT_FALSE(segment.sha256.empty());
        EXPECT_GT(segment.bytesWritten, 0);
        EXPECT_TRUE(VideoDecoder::open(segment.path).ok())
            << segment.path.string() << " does not decode";
    }
}

TEST(CameraCapture, ABluetoothSourceIsRefusedBeforeAnyFileIsCreated) {
    TemporaryDirectory scratch("trace-capture-bt");
    CameraSource camera;
    camera.id = "cam-bt";
    camera.name = "HERO12 Black";
    camera.transport = CameraTransport::BluetoothControl;
    camera.link = CameraLink::Bluetooth;
    camera.address = "AA:BB:CC:DD:EE:FF";

    CaptureSession session;
    auto recorded = session.record(camera, scratch.path(), "cam");
    ASSERT_FALSE(recorded.ok());
    EXPECT_EQ(recorded.error().code(), ErrorCode::Unsupported);

    // Nothing was created. An empty file left behind would look to an operator
    // like a recording that captured nothing rather than one that never began.
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(scratch.path())) {
        (void)entry;
        ++entries;
    }
    EXPECT_EQ(entries, 0);
}

TEST(CameraCapture, AnAddressThatWasNeverReachableFailsNowNotAfterTheRetryWindow) {
    TemporaryDirectory scratch("trace-capture-badaddress");
    // Port 9 is discard; nothing serves video there.
    auto camera = cameraSourceFromUri("http://127.0.0.1:9/live");
    ASSERT_TRUE(camera.ok());

    CaptureSession session;
    CaptureSettings settings;
    settings.openTimeoutSeconds = 2;
    settings.reconnectWindowMs = 30'000;  // deliberately long
    settings.reconnectDelayMs = 500;

    const auto start = std::chrono::steady_clock::now();
    auto recorded = session.record(camera.take(), scratch.path(), "cam", settings);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(recorded.ok());
    // A source that has never worked is a wrong address, not a drop. Retrying it
    // for thirty seconds would leave an operator watching a capture that was
    // never going to start.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 15);
}

// ------------------------------------------------------------- provenance

TEST(CaptureProvenance, SaysThereIsNoOriginalAndCarriesNoCredentials) {
    auto parsed = cameraSourceFromUri("rtsp://operator:hunter2@192.0.2.10:554/stream1", "Front door");
    ASSERT_TRUE(parsed.ok());

    CaptureSegment segment;
    segment.startedAt = "2026-09-01T12:00:00Z";
    segment.endedAt = "2026-09-01T12:00:30Z";
    segment.durationUs = 30'000'000;
    segment.framesWritten = 750;
    segment.bytesWritten = 1'024'000;
    segment.sha256 = std::string(64, 'a');
    segment.complete = true;

    CaptureOutcome outcome;
    outcome.segments.push_back(segment);
    outcome.wallClockMs = 30'000;

    const std::string json =
        captureProvenance(parsed.value(), segment, outcome, std::nullopt).dump();

    // The claim that distinguishes a capture from an import, stated rather than
    // left to be inferred from a missing field.
    EXPECT_NE(json.find("no_original_exists"), std::string::npos);
    EXPECT_NE(json.find("first generation"), std::string::npos);
    EXPECT_NE(json.find(segment.sha256), std::string::npos);
    EXPECT_NE(json.find("network_stream"), std::string::npos);

    // A capture record is read by people who are not the operator.
    EXPECT_EQ(json.find("hunter2"), std::string::npos);
}

TEST(CaptureProvenance, EverySegmentCarriesWhetherTheCaptureWasContinuous) {
    auto parsed = cameraSourceFromUri("rtsp://192.0.2.10:554/stream1");
    ASSERT_TRUE(parsed.ok());

    CaptureSegment segment;
    segment.startedAt = "2026-09-01T12:01:00Z";
    segment.endedAt = "2026-09-01T12:01:30Z";
    segment.durationUs = 30'000'000;
    segment.sha256 = std::string(64, 'b');

    CaptureOutcome outcome;
    outcome.segments.push_back(segment);
    outcome.gaps.push_back(CaptureGap{15'000'000, 4'200, "camera reconnected"});

    const CaptureGap preceding = outcome.gaps.front();
    const std::string json = captureProvenance(parsed.value(), segment, outcome, preceding).dump();

    // An examiner holding one exhibit must be able to see the recording it came
    // from had a gap, without having to find the other exhibits first.
    EXPECT_NE(json.find("capture_continuous"), std::string::npos);
    EXPECT_NE(json.find("preceded_by_gap"), std::string::npos);
    EXPECT_NE(json.find("camera reconnected"), std::string::npos);
}

// --------------------------------------------------- capture into a case

TEST(CaptureService, FilesEachSegmentAsEvidenceWithCaptureProvenance) {
    TemporaryDirectory root("trace-capture-service");
    auto stack = testing::TestStack::create(root.path());

    CaseDraft draft;
    draft.title = "Camera capture";
    auto created = stack.cases->createCase(draft);
    ASSERT_TRUE(created.ok()) << created.error().toString();
    const Case caseRecord = created.take();

    StreamServer server;
    StreamServer::Options options;
    options.chunkDelayMs = 3;
    ASSERT_TRUE(server.start(testing::sampleVideoPath(), options));

    auto camera = cameraSourceFromUri(server.url(), "Test stream");
    ASSERT_TRUE(camera.ok());

    CaptureService service(*stack.layout, *stack.evidence, stack.derivedAssets, stack.audit);

    CaptureRegistration request;
    request.caseId = caseRecord.id;
    request.caseNumber = caseRecord.caseNumber;
    request.camera = camera.take();
    request.description = "Front entrance";
    request.settings.maximumDurationMs = 20'000;
    request.settings.openTimeoutSeconds = 5;
    request.settings.reconnectWindowMs = 500;

    auto filed = service.capture(request);
    ASSERT_TRUE(filed.ok()) << filed.error().toString();
    const CaptureRegistrationOutcome outcome = filed.take();

    ASSERT_FALSE(outcome.items.empty()) << "the capture filed nothing";
    EXPECT_TRUE(outcome.unregistered.empty());

    const Evidence& evidence = outcome.items.front().evidence;
    EXPECT_FALSE(evidence.id.empty());
    EXPECT_FALSE(evidence.evidenceNumber.empty());
    EXPECT_FALSE(evidence.sha256.empty());
    EXPECT_GT(evidence.fileSize, 0);

    // The managed original is there and decodes.
    const auto managed = stack.evidence->absolutePath(evidence);
    ASSERT_TRUE(std::filesystem::exists(managed));
    EXPECT_TRUE(VideoDecoder::open(managed).ok());

    // The staging copy is gone: keeping it would leave a second copy of the
    // recording outside managed storage.
    EXPECT_FALSE(std::filesystem::exists(outcome.items.front().segment.path));

    // And the provenance says how it got there.
    auto operations = stack.derivedAssets->operationsForEvidence(evidence.id);
    ASSERT_TRUE(operations.ok()) << operations.error().toString();
    ASSERT_FALSE(operations.value().empty()) << "a captured exhibit has no capture provenance";
    const auto& operation = operations.value().front();
    EXPECT_EQ(operation.operationType, "camera_capture");
    EXPECT_NE(operation.parametersJson.find("no_original_exists"), std::string::npos);
    EXPECT_FALSE(operation.notes.empty());
    EXPECT_FALSE(operation.softwareVersion.empty());
}

TEST(CaptureService, ARefusedCaptureIsRecordedAgainstTheCase) {
    TemporaryDirectory root("trace-capture-refused");
    auto stack = testing::TestStack::create(root.path());

    CaseDraft draft;
    draft.title = "Bluetooth attempt";
    auto created = stack.cases->createCase(draft);
    ASSERT_TRUE(created.ok()) << created.error().toString();
    const Case caseRecord = created.take();

    CaptureService service(*stack.layout, *stack.evidence, stack.derivedAssets, stack.audit);

    CaptureRegistration request;
    request.caseId = caseRecord.id;
    request.caseNumber = caseRecord.caseNumber;
    request.camera.id = "cam-bt";
    request.camera.name = "HERO12 Black";
    request.camera.transport = CameraTransport::BluetoothControl;
    request.camera.link = CameraLink::Bluetooth;
    request.camera.address = "AA:BB:CC:DD:EE:FF";

    auto filed = service.capture(request);
    ASSERT_FALSE(filed.ok());
    EXPECT_EQ(filed.error().code(), ErrorCode::Unsupported);

    // The attempt is in the trail. A refusal one layer down would leave no
    // record that an operator tried to record from a camera and got nothing.
    AuditQuery query;
    query.caseId = caseRecord.id;
    auto events = stack.audit->list(query);
    ASSERT_TRUE(events.ok()) << events.error().toString();
    const bool recorded =
        std::any_of(events.value().begin(), events.value().end(), [](const AuditEvent& event) {
            return event.action == AuditAction::CaptureFailed;
        });
    EXPECT_TRUE(recorded) << "the refused capture left no audit record";
}

}  // namespace
}  // namespace trace
