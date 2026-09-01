#include "media/capture/bluetooth.h"

#include <filesystem>
#include <system_error>

namespace trace {
namespace bluetooth {
namespace {

/// The single sentence every "why can't I use Bluetooth" path returns. Kept as
/// one constant so the interface, the log line and the documentation cannot
/// drift into saying three different things about the same limitation.
constexpr const char* kNoVideoOverBluetooth =
    "Bluetooth cannot carry video. There is no Bluetooth video profile in general "
    "use, and Bluetooth Low Energy's throughput is roughly one to two orders of "
    "magnitude below even heavily compressed 1080p. Cameras that advertise "
    "\"Bluetooth\" use it to be woken and commanded; the video then arrives over "
    "WiFi, which TRACE records as an ordinary network camera.";

/// Why every entry point below refuses, in the operator's terms. This is a
/// deliberate refusal rather than an empty result: "TRACE has no Bluetooth
/// backend" and "there are no cameras in the room" are different facts, and a
/// caller shown an empty list would act on the wrong one.
constexpr const char* kNoBackend =
    "This build has no Bluetooth backend. BLE requires a separate integration per "
    "platform (BlueZ on Linux, WinRT on Windows, CoreBluetooth on macOS) and none "
    "is compiled in.";

/// Detecting an adapter, per platform, without a Bluetooth stack linked in.
///
/// On Linux the kernel exposes every registered HCI adapter as a directory under
/// /sys/class/bluetooth, so the presence of that class directory with at least
/// one entry is a reliable answer that costs one stat and needs no library. The
/// other platforms have no equivalent that can be read without their SDK, so
/// they report "not determined" rather than a guess: claiming there is no
/// adapter when TRACE simply cannot see one would send an operator looking for
/// hardware they already have.
struct AdapterProbe {
    bool present = false;
    std::string detail;
};

AdapterProbe probeAdapter() {
    AdapterProbe probe;
#if defined(__linux__)
    std::error_code ec;
    const std::filesystem::path sysfs{"/sys/class/bluetooth"};
    if (!std::filesystem::is_directory(sysfs, ec)) {
        probe.detail = "No /sys/class/bluetooth: the kernel has no Bluetooth adapter registered.";
        return probe;
    }
    int adapters = 0;
    for (std::filesystem::directory_iterator it(sysfs, ec), end; !ec && it != end; it.increment(ec)) {
        ++adapters;
    }
    if (adapters > 0) {
        probe.present = true;
        probe.detail = "Adapter registered with the kernel.";
    } else {
        probe.detail = "/sys/class/bluetooth is empty: no adapter registered.";
    }
#elif defined(_WIN32)
    probe.detail =
        "Adapter presence is not determined on Windows without the WinRT radio API, "
        "which this build does not link.";
#elif defined(__APPLE__)
    probe.detail =
        "Adapter presence is not determined on macOS without CoreBluetooth, which "
        "this build does not link.";
#else
    probe.detail = "Adapter presence is not determined on this platform.";
#endif
    return probe;
}

}  // namespace

Availability availability() {
    Availability result;
    // There is no TRACE_WITH_BLUETOOTH today. When a backend is added it sets
    // this, and the two halves of the answer stay independently true: a build
    // with a backend on a machine with no dongle must still say so.
#if defined(TRACE_WITH_BLUETOOTH)
    result.backendCompiled = true;
#else
    result.backendCompiled = false;
#endif

    const AdapterProbe probe = probeAdapter();
    result.adapterPresent = probe.present;

    if (!result.backendCompiled) {
        result.detail = std::string(kNoBackend) + " " + probe.detail;
    } else if (!result.adapterPresent) {
        result.detail = probe.detail;
    } else {
        result.detail = "Bluetooth backend available. " + probe.detail;
    }
    return result;
}

Result<std::vector<AdvertisedCamera>> scan(int timeoutMs) {
    (void)timeoutMs;
    const Availability state = availability();
    return Result<std::vector<AdvertisedCamera>>::failure(
        ErrorCode::Unsupported, "Bluetooth scanning is not available in this build.", state.detail);
}

const char* toString(Command command) {
    switch (command) {
        case Command::WakeUp:         return "wake_up";
        case Command::StartRecording: return "start_recording";
        case Command::StopRecording:  return "stop_recording";
        case Command::EnableWifi:     return "enable_wifi";
        case Command::DisableWifi:    return "disable_wifi";
        case Command::ReadStatus:     return "read_status";
    }
    return "unknown";
}

Result<std::unique_ptr<ControlLink>> connect(const AdvertisedCamera& camera) {
    const Availability state = availability();
    // Names the camera the caller asked for, because an operator who selected a
    // device from a list needs to see that this is about that device and not a
    // general failure of the panel they are looking at.
    std::string detail = state.detail;
    if (!camera.address.empty()) {
        detail = "Requested camera " + camera.address + ". " + detail;
    }
    return Result<std::unique_ptr<ControlLink>>::failure(
        ErrorCode::Unsupported, "Cannot connect to a Bluetooth camera in this build.", detail);
}

const char* videoOverBluetoothExplanation() { return kNoVideoOverBluetooth; }

}  // namespace bluetooth
}  // namespace trace
