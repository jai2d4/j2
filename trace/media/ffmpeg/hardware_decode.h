#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/common/json.h"
#include "core/common/result.h"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;

namespace trace {

/// Hardware-accelerated video decoding, and the reasons it is off by default.
///
/// ## What this is for
///
/// Software decode of a 4K recording runs at a few frames a second on one core.
/// An analyst scrubbing through six hours of it, or an analysis pass sampling
/// every frame, is waiting on the decoder and nothing else. Handing that to a
/// GPU or a fixed-function decoder is the difference between a review that
/// happens and one that does not.
///
/// ## Why it is off by default anyway
///
/// A hardware decoder is a *different implementation* of the same standard.
/// H.264 and HEVC are specified to produce bit-exact output, and conformant
/// decoders do — but "conformant" is a claim about a particular chip and driver,
/// not something TRACE can verify on the machine it is running on. If a
/// hardware path produced even slightly different pixels, then a frame exported
/// as an exhibit would depend on which decoder happened to be enabled, and two
/// analysts examining the same recording could be looking at different images.
///
/// That is not a hypothetical risk worth accepting for speed, so:
///
/// - The setting defaults to **off**.
/// - `verifyMatchesSoftware()` exists so that a deployment can check the claim
///   on its own hardware, against its own footage, rather than trusting it.
/// - Every derived asset records which decoder produced it, so a frame exported
///   through a hardware path says so.
///
/// ## What has not been done
///
/// **None of this has run on a GPU.** The machine TRACE was built on has no
/// accelerator of any kind, so every code path below is written and unexecuted.
/// `docs/HARDWARE_DECODE.md` says which claims are measured (none about
/// throughput) and what to check on real hardware. No performance figure appears
/// anywhere in this codebase, because none has been observed.
namespace hwaccel {

/// One accelerator this build could use, as reported by FFmpeg.
struct Device {
    /// FFmpeg's own name: "cuda", "vaapi", "d3d11va", "videotoolbox", "qsv"…
    std::string name;
    /// What the operator sees.
    std::string displayName;
    /// True when a device of this type could actually be opened, not merely
    /// compiled in. The distinction matters: FFmpeg builds usually contain every
    /// accelerator, and only trying tells you whether one is present.
    bool available = false;
    /// Why it could not be opened, when it could not.
    std::string unavailableReason;
};

/// Every accelerator this FFmpeg build knows about, each probed by opening it.
///
/// Probing costs a device open per type and is done once per process; the result
/// is cached. A machine with no accelerator gets a list where every entry is
/// unavailable and says why, which is what the settings dialog shows rather than
/// an empty box.
const std::vector<Device>& devices();

/// The accelerators that opened successfully.
std::vector<Device> availableDevices();

/// Whether any accelerator is usable here.
bool anyAvailable();

/// Picks an accelerator for a codec, preferring the platform's native one.
///
/// Returns an empty string when nothing suitable exists, which is not an error —
/// it is the ordinary answer on most machines, and the caller decodes in
/// software.
std::string preferredDeviceFor(const AVCodec* codec);

/// Attaches a hardware device to a decoder that has not been opened yet.
///
/// On success the context decodes into hardware frames, and `receiveFrame` has
/// to be used to get them into system memory. On failure the context is left
/// exactly as it was, so the caller can open it for software decoding without
/// having to rebuild it — a fallback that required a second allocation would be
/// a fallback nobody trusted.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// `deviceName` is one of `devices()`. Fails when the device cannot be
    /// opened or the codec has no hardware configuration for it.
    Status attach(AVCodecContext* codec, const AVCodec* decoder, const std::string& deviceName);

    /// True when a device is attached and frames will arrive in hardware memory.
    bool active() const;

    /// The device in use, or empty.
    const std::string& deviceName() const;

    /// Moves `source` into system memory if it is a hardware frame, leaving it
    /// alone if it is not.
    ///
    /// `destination` is filled and must be reusable across calls. A transfer
    /// failure is a hard error rather than something to paper over: silently
    /// returning the previous frame would show an analyst one moment of a
    /// recording while telling them it was another.
    Status transfer(AVFrame* source, AVFrame* destination, AVFrame** usable);

    /// For provenance. Empty when software decoding.
    JsonValue describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Decodes the same frames both ways and reports whether they are identical.
///
/// This exists because the honest answer to "does hardware decode change the
/// evidence" is "check it on your hardware" — and a claim like that is worth
/// nothing unless the software provides the check. It decodes the first
/// `frameCount` frames of `file` in software and again through `deviceName`, and
/// compares the resulting images byte for byte.
struct ComparisonOutcome {
    int framesCompared = 0;
    int framesIdentical = 0;
    /// Largest absolute difference of any single channel value, across all
    /// frames. Zero means bit-exact.
    int maximumChannelDifference = 0;
    /// Mean absolute difference over every channel of every compared frame.
    /// Reported alongside the maximum because one stray pixel and a uniformly
    /// shifted image are very different findings.
    double meanChannelDifference = 0.0;
    std::string deviceName;
    std::string softwareDecoder;

    bool identical() const {
        return framesCompared > 0 && framesIdentical == framesCompared &&
               maximumChannelDifference == 0;
    }
};

Result<ComparisonOutcome> verifyMatchesSoftware(const std::filesystem::path& file,
                                                const std::string& deviceName,
                                                int frameCount = 12);

}  // namespace hwaccel
}  // namespace trace
