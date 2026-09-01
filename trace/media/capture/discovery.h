#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "media/capture/camera.h"

namespace trace {

/// Finding cameras, by each transport's own means.
///
/// Three mechanisms, because there is no single one that covers the ground:
///
/// - **Network cameras** answer ONVIF WS-Discovery, a SOAP probe multicast to
///   239.255.255.250:3702. Most professional IP cameras implement it; consumer
///   ones often do not, which is why manual entry is a first-class path rather
///   than a fallback for when discovery "fails".
/// - **Attached cameras** are enumerated from the platform's device list —
///   /dev/video* on Linux, DirectShow on Windows.
/// - **Bluetooth cameras** advertise over BLE. What is discovered there is a
///   camera to *talk to*, not a camera to record from; see `bluetooth.h`.
///
/// Discovery finds candidates. It does not verify that any of them will stream:
/// that is `probeCamera`, and keeping the two separate means a camera that
/// appears in the list and then refuses to open produces a specific error rather
/// than being quietly dropped from the results.
namespace discovery {

struct Options {
    /// How long to listen for ONVIF replies. Cameras answer within a second or
    /// two; the default trades a little latency for not missing a slow one.
    int networkTimeoutMs = 3000;
    /// Include attached devices.
    bool includeLocalDevices = true;
    /// Include network cameras.
    bool includeNetworkCameras = true;
};

/// Everything found, plus what could not be looked for and why.
///
/// The second half matters: an empty list because nothing answered and an empty
/// list because the machine has no network are the same to a caller that only
/// gets a vector, and they need different actions from an operator.
struct Outcome {
    std::vector<CameraSource> cameras;
    /// Transports that could not be searched, with the reason.
    std::vector<std::pair<std::string, std::string>> unavailable;

    bool anyFound() const { return !cameras.empty(); }
};

/// Searches every enabled transport. Blocks for up to `networkTimeoutMs`.
Result<Outcome> findCameras(const Options& options = {});

/// ONVIF WS-Discovery only. Exposed separately so a caller can rescan just the
/// network without re-enumerating local hardware.
Result<std::vector<CameraSource>> findNetworkCameras(int timeoutMs = 3000);

/// Attached capture devices. Empty, not an error, on a machine with none.
Result<std::vector<CameraSource>> findLocalDevices();

}  // namespace discovery
}  // namespace trace
