#include "media/capture/camera.h"

#include <algorithm>
#include <array>
#include <cctype>

#include "core/security/sha256.h"

namespace trace {
namespace {

/// Schemes FFmpeg can demux a live camera from in the builds TRACE ships.
/// Checked explicitly rather than passed through, so an unsupported scheme is
/// refused where the operator typed it instead of failing later as an opaque
/// "Invalid data found when processing input".
constexpr std::array<const char*, 8> kNetworkSchemes = {
    "rtsp://", "rtsps://", "rtmp://", "rtmps://", "srt://", "http://", "https://", "rtp://",
};

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string lowered(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// A short stable identifier for an endpoint, so the same camera keeps the same
/// id across a rescan without the id being the address itself — which would put
/// a credential-bearing string into every log line that mentions it.
std::string identifierFor(const std::string& endpoint) {
    return "cam-" + Sha256::hash(endpoint).substr(0, 12);
}

}  // namespace

const char* toString(CameraTransport transport) {
    switch (transport) {
        case CameraTransport::LocalDevice:      return "local_device";
        case CameraTransport::NetworkStream:    return "network_stream";
        case CameraTransport::BluetoothControl: return "bluetooth_control";
    }
    return "network_stream";
}

const char* toDisplayString(CameraTransport transport) {
    switch (transport) {
        case CameraTransport::LocalDevice:      return "Attached device";
        case CameraTransport::NetworkStream:    return "Network camera";
        // Named for what it does, so nothing in the interface implies video
        // arrives this way.
        case CameraTransport::BluetoothControl: return "Bluetooth control link";
    }
    return "Network camera";
}

CameraTransport cameraTransportFromString(const std::string& text, CameraTransport fallback) {
    if (text == "local_device") return CameraTransport::LocalDevice;
    if (text == "network_stream") return CameraTransport::NetworkStream;
    if (text == "bluetooth_control") return CameraTransport::BluetoothControl;
    return fallback;
}

bool carriesVideo(CameraTransport transport) {
    // The whole reason this function exists. Bluetooth has no video profile in
    // general use and BLE's throughput is short of compressed 1080p by one to
    // two orders of magnitude; a BLE link is how a camera is told to start
    // streaming somewhere else, not where the stream arrives.
    return transport != CameraTransport::BluetoothControl;
}

const char* toString(CameraLink link) {
    switch (link) {
        case CameraLink::Unknown:   return "unknown";
        case CameraLink::Wired:     return "wired";
        case CameraLink::Wireless:  return "wireless";
        case CameraLink::Bluetooth: return "bluetooth";
    }
    return "unknown";
}

const char* toDisplayString(CameraLink link) {
    switch (link) {
        case CameraLink::Unknown:   return "Link not known";
        case CameraLink::Wired:     return "Wired";
        case CameraLink::Wireless:  return "WiFi";
        case CameraLink::Bluetooth: return "Bluetooth";
    }
    return "Link not known";
}

std::string redactCredentials(const std::string& uri) {
    const auto schemeEnd = uri.find("://");
    if (schemeEnd == std::string::npos) return uri;

    const auto authorityStart = schemeEnd + 3;
    // The at-sign has to be looked for before the first slash of the path:
    // a path or query may legitimately contain one, and cutting there would
    // mangle the URI rather than redact it.
    const auto pathStart = uri.find('/', authorityStart);
    const auto searchEnd = pathStart == std::string::npos ? uri.size() : pathStart;
    const auto at = uri.rfind('@', searchEnd);
    if (at == std::string::npos || at < authorityStart) return uri;

    return uri.substr(0, authorityStart) + "<credentials>@" + uri.substr(at + 1);
}

std::string CameraSource::redactedUri() const { return redactCredentials(uri); }

JsonValue CameraSource::toJson() const {
    JsonValue value = JsonValue::object()
                          .set("id", id)
                          .set("name", name)
                          .set("transport", toString(transport))
                          .set("link", toString(link))
                          // The redacted form, always. A camera URI routinely
                          // carries a password, and this object is written into
                          // provenance and logs.
                          .set("uri", redactedUri())
                          .set("carries_video", carriesVideo(transport));
    value.setIfNotEmpty("manufacturer", manufacturer);
    value.setIfNotEmpty("model", model);
    value.setIfNotEmpty("address", address);
    if (hasEmbeddedCredentials) value.set("credentials_embedded", true);
    if (!reportedProfiles.empty()) {
        JsonValue profiles = JsonValue::array();
        for (const std::string& profile : reportedProfiles) profiles.push(profile);
        value.set("reported_profiles", profiles);
    }
    return value;
}

Result<CameraSource> cameraSourceFromUri(const std::string& uri, const std::string& name) {
    using ResultType = Result<CameraSource>;
    if (uri.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "No camera address was given");
    }

    CameraSource source;
    source.uri = uri;
    const std::string lower = lowered(uri);

    // A local device node or a DirectShow/AVFoundation specifier.
    if (startsWith(lower, "/dev/") || startsWith(lower, "video=") ||
        startsWith(lower, "device:") || startsWith(lower, "avfoundation:")) {
        source.transport = CameraTransport::LocalDevice;
        // Attached is attached: USB, a capture card and an SDI bridge all reach
        // the machine over something physical.
        source.link = CameraLink::Wired;
        source.address = uri;
        source.id = identifierFor(uri);
        source.name = name.empty() ? uri : name;
        return ResultType::success(std::move(source));
    }

    if (startsWith(lower, "bluetooth://") || startsWith(lower, "ble://")) {
        source.transport = CameraTransport::BluetoothControl;
        source.link = CameraLink::Bluetooth;
        source.address = uri;
        source.id = identifierFor(uri);
        source.name = name.empty() ? uri : name;
        // Deliberately allowed to be constructed: a BLE camera is a real thing
        // to have in a list. Opening it for video is what fails, and it fails
        // with a reason rather than a decode error.
        return ResultType::success(std::move(source));
    }

    const bool network = std::any_of(
        kNetworkSchemes.begin(), kNetworkSchemes.end(),
        [&lower](const char* scheme) { return startsWith(lower, scheme); });
    if (!network) {
        return ResultType::failure(
            ErrorCode::Unsupported, "TRACE does not know how to reach that camera",
            "Give an rtsp://, rtsps://, rtmp://, rtmps://, srt://, http(s):// or rtp:// address, "
            "a local device such as /dev/video0, or a bluetooth:// control link.");
    }

    source.transport = CameraTransport::NetworkStream;
    // Not inferable from the URI. An RTSP camera looks identical over Ethernet
    // and over WiFi, which is the point — but that means TRACE must not claim
    // to know which it was. A caller that does know sets it.
    source.link = CameraLink::Unknown;
    source.address = redactCredentials(uri);
    source.hasEmbeddedCredentials = source.address != uri;
    source.id = identifierFor(source.address);
    source.name = name.empty() ? source.address : name;
    return ResultType::success(std::move(source));
}

}  // namespace trace
