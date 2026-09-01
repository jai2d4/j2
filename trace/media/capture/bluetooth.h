#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "media/capture/camera.h"

namespace trace {

/// Bluetooth as cameras actually use it: a control channel, not a video one.
///
/// ## Why there is no Bluetooth video path here
///
/// TRACE was asked to connect to cameras over cable, WiFi and Bluetooth. Two of
/// those carry video. This one does not, and building something that looked like
/// it did would be the worst outcome available — an operator would point it at a
/// scene and get nothing, or worse, believe they were recording.
///
/// The reasons are not TRACE's to work around:
///
/// - **There is no Bluetooth video profile in general use.** Bluetooth's
///   published profiles cover audio (A2DP), serial data (SPP), input devices and
///   file transfer. Video was specified once, as VDP over Bluetooth Classic, and
///   is not implemented by cameras anyone deploys.
/// - **Bluetooth Low Energy has nowhere near the throughput.** BLE's practical
///   application data rate is on the order of a megabit per second, and often a
///   fraction of that in real conditions. Heavily compressed 1080p is several
///   megabits per second. The gap is one to two orders of magnitude, and it is
///   not a matter of tuning.
///
/// What cameras genuinely do over Bluetooth is the interesting part, and it is
/// what this supports: an action camera, a body-worn camera or a dashcam
/// advertises over BLE, and a phone or workstation uses that link to wake it,
/// read its battery and storage, change settings, start and stop recording, and
/// — the step that matters here — **tell it to bring up its WiFi**. The video
/// then arrives over that WiFi link, as an ordinary network stream, through
/// exactly the same code path as a wired IP camera.
///
/// So Bluetooth's place in TRACE is: find the camera, command it, and hand back
/// the network address it starts streaming on. That is a real capability and it
/// is described as what it is.
///
/// ## What is here and what is not
///
/// The transport model, the discovery interface and the control vocabulary are
/// defined. The platform backends are **not implemented**: BlueZ on Linux,
/// WinRT on Windows and CoreBluetooth on macOS are three separate integrations
/// against three unrelated APIs, and the machine TRACE was built on has no
/// Bluetooth adapter at all — nothing written against them could have been run
/// even once. Every entry point below therefore reports that plainly rather than
/// returning an empty list that reads as "no cameras nearby".
namespace bluetooth {

/// Whether this build has a Bluetooth backend compiled in, and whether the
/// machine has an adapter. Both are false today; the two are kept separate
/// because they need different answers from an operator.
struct Availability {
    bool backendCompiled = false;
    bool adapterPresent = false;
    std::string detail;

    bool usable() const { return backendCompiled && adapterPresent; }
};

Availability availability();

/// A camera advertising over BLE.
struct AdvertisedCamera {
    std::string address;      ///< BD_ADDR, or a platform handle on macOS
    std::string name;         ///< the advertised local name
    int rssi = 0;             ///< signal strength, for telling near from far
    std::vector<std::string> serviceUuids;
};

/// Scans for cameras advertising over BLE.
///
/// Reports Unsupported today. It does not return an empty list: "no backend" and
/// "nothing nearby" are different facts and only one of them is about the room.
Result<std::vector<AdvertisedCamera>> scan(int timeoutMs = 5000);

/// What a control link can be asked to do. Named after the real operations
/// action and body cameras expose over BLE.
enum class Command {
    WakeUp,
    StartRecording,
    StopRecording,
    EnableWifi,     ///< the one that leads to a video stream
    DisableWifi,
    ReadStatus,     ///< battery, storage, recording state
};

const char* toString(Command command);

/// State a camera reports back over the control link.
struct CameraStatus {
    int batteryPercent = -1;      ///< -1 when not reported
    std::int64_t storageFreeBytes = -1;
    bool recording = false;
    std::string firmware;
};

/// An open BLE control link to one camera.
class ControlLink {
public:
    virtual ~ControlLink() = default;

    virtual Status send(Command command) = 0;
    virtual Result<CameraStatus> status() = 0;

    /// Asks the camera to start streaming and returns where to capture from.
    ///
    /// This is the bridge between the two transports, and the reason Bluetooth
    /// is in TRACE at all: it typically issues EnableWifi, waits for the camera
    /// to bring up its access point, and returns the RTSP address it serves.
    /// The result is an ordinary NetworkStream camera that CaptureSession
    /// records exactly like any other.
    virtual Result<CameraSource> beginStreaming() = 0;
};

/// Connects to an advertised camera.
///
/// Reports Unsupported today, for the reasons in the file header.
Result<std::unique_ptr<ControlLink>> connect(const AdvertisedCamera& camera);

/// The sentence TRACE shows an operator who asks for Bluetooth video, in one
/// place so the interface, the logs and the documentation cannot drift apart.
const char* videoOverBluetoothExplanation();

}  // namespace bluetooth
}  // namespace trace
