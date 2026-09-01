#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common/json.h"
#include "core/common/result.h"

namespace trace {

/// How TRACE reaches a camera.
///
/// ## Cable, WiFi and Bluetooth are not three equivalent options
///
/// The request that produced this file was "hook up to any camera system via
/// solid cable, WiFi, Bluetooth". Two of those three are real video transports
/// and one is not, and pretending otherwise would produce a feature that looks
/// finished and cannot work:
///
/// - **Cable** means two different things. An Ethernet cable to an IP camera is
///   a *network* transport, and TRACE reaches it exactly as it reaches the same
///   camera over WiFi — RTSP over IP either way. A USB cable to a webcam or an
///   HDMI capture card is a *local device*, a different code path entirely.
/// - **WiFi** is, to everything above the network stack, identical to Ethernet.
///   The camera has an address and speaks RTSP. What actually differs is
///   discovery and how often the link drops, not the protocol, so TRACE does not
///   pretend to have two implementations where it has one.
/// - **Bluetooth cannot carry video.** There is no Bluetooth video profile in
///   general use, and BLE's practical throughput — a couple of megabits a second
///   at best, usually far less — is one to two orders of magnitude short of even
///   heavily compressed 1080p. What Bluetooth is genuinely used for on cameras
///   is *control*: an action camera is woken, configured and told to start
///   recording over BLE, and then brings up its own WiFi access point for the
///   video. TRACE models it that way, and `carriesVideo()` returns false for it.
///
/// So the honest taxonomy is by what the transport can do, not by what cable or
/// radio is involved.
enum class CameraTransport {
    /// A camera attached to this machine: USB webcam, capture card, DV.
    /// video4linux2 on Linux, DirectShow on Windows, AVFoundation on macOS.
    LocalDevice,
    /// An IP camera reached over the network, whether the last hop is an
    /// Ethernet cable or WiFi. RTSP, RTMP, HTTP/MJPEG or SRT.
    NetworkStream,
    /// A Bluetooth Low Energy link. Control and telemetry only — never video.
    /// Used to discover, pair with and command a camera that then streams over
    /// one of the transports above.
    BluetoothControl,
};

const char* toString(CameraTransport transport);
const char* toDisplayString(CameraTransport transport);
CameraTransport cameraTransportFromString(const std::string& text,
                                          CameraTransport fallback = CameraTransport::NetworkStream);

/// Whether a transport can deliver video frames at all.
///
/// The one place this is false is Bluetooth, and every code path that opens a
/// stream checks it rather than discovering the problem as a decode failure
/// somewhere less obvious.
bool carriesVideo(CameraTransport transport);

/// How the link physically reaches the camera, when that is known.
///
/// Recorded because it belongs in the provenance of a recording — "captured
/// over WiFi" and "captured over a cable" are different claims about how
/// interruptible the capture was — but it deliberately does not affect which
/// code path runs. An IP camera is opened the same way on either.
enum class CameraLink {
    Unknown,
    Wired,      ///< Ethernet, USB, SDI/HDMI via a capture card
    Wireless,   ///< WiFi
    Bluetooth,  ///< BLE control link
};

const char* toString(CameraLink link);
const char* toDisplayString(CameraLink link);

/// A camera TRACE can open, however it was found.
struct CameraSource {
    /// Stable within a session. For network cameras this is derived from the
    /// endpoint so the same camera reappears with the same id across a rescan.
    std::string id;
    /// What an operator sees. From ONVIF metadata, the device node, or the
    /// address when nothing better is known.
    std::string name;
    /// Manufacturer and model where the camera reported them; empty otherwise.
    /// Never guessed from the address.
    std::string manufacturer;
    std::string model;

    CameraTransport transport = CameraTransport::NetworkStream;
    CameraLink link = CameraLink::Unknown;

    /// What gets handed to FFmpeg. An RTSP/RTMP/HTTP/SRT URL for a network
    /// camera, a device node such as /dev/video0 for a local one, empty for a
    /// Bluetooth control link that has not yet been asked for a stream.
    std::string uri;

    /// The address a network camera answers on, without credentials. Kept
    /// separate from `uri` so it can be logged and displayed: a URI may carry a
    /// password, and this never does.
    std::string address;

    /// True when the URI carries embedded credentials. Nothing displays or logs
    /// such a URI, and this is how callers know to redact rather than having to
    /// re-parse it.
    bool hasEmbeddedCredentials = false;

    /// Filled in only when the camera actually told us. An empty vector means
    /// "not reported", never "none".
    std::vector<std::string> reportedProfiles;

    bool valid() const { return !id.empty() && (!uri.empty() || !address.empty()); }

    /// A form safe to log or show: credentials removed.
    std::string redactedUri() const;

    JsonValue toJson() const;
};

/// Parses and validates a camera URI, and says what kind it is.
///
/// Rejects anything that is not a transport TRACE actually supports rather than
/// handing it to FFmpeg to fail on later with a less useful message. A rejected
/// URI names what was wrong with it.
Result<CameraSource> cameraSourceFromUri(const std::string& uri, const std::string& name = {});

/// Strips a `user:password@` section from a URL, leaving it otherwise intact.
/// Returns the input unchanged when there is nothing to remove.
std::string redactCredentials(const std::string& uri);

}  // namespace trace
