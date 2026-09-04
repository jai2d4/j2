#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/common/time_utils.h"
#include "media/capture/camera.h"

namespace trace {

/// Records a live camera to disk.
///
/// ## Capturing is not ingesting, and the difference is the whole point
///
/// Every other path into TRACE receives a file that already exists: it is
/// copied into managed storage, hashed, and the hash is the thing an outside
/// party can check against the original. The chain of custody starts before
/// TRACE.
///
/// A live camera has no original. **TRACE is the recording device**, and the
/// chain of custody starts here — with this process, on this machine, at this
/// clock. That changes what can honestly be claimed about the result:
///
/// - There is nothing to compare the hash against. It is computed when the
///   segment closes and it attests that the file has not changed *since*, not
///   that it matches something that existed before.
/// - The provenance says captured, not ingested, and names the camera, the
///   transport and the machine clock. `EvidenceService::ingest` would record the
///   wrong story.
/// - The recording is only as trustworthy as the link that carried it, which is
///   why a dropped connection is recorded rather than papered over.
///
/// ## Gaps are recorded, never closed
///
/// A network camera drops. When it does, this does not stitch the two halves
/// together into a file that looks continuous: it ends the segment, records the
/// gap with its start, end and cause, and begins a new segment when the camera
/// comes back. A recording that silently omits ninety seconds while presenting
/// continuous timestamps is worse than one that stops, because nothing about it
/// looks wrong.
struct CaptureGap {
    Microseconds atUs = 0;        ///< where in the capture the link failed
    std::int64_t durationMs = 0;  ///< how long it was gone, as measured by the wall clock
    std::string reason;
};

/// One continuous recording, from one camera, with no gaps inside it.
struct CaptureSegment {
    std::filesystem::path path;
    std::string startedAt;   ///< ISO-8601 UTC, machine clock at first packet
    std::string endedAt;
    Microseconds durationUs = 0;
    std::int64_t bytesWritten = 0;
    std::int64_t framesWritten = 0;
    /// Packets the camera sent with no usable timestamp, placed on the
    /// recording's own timeline instead. Recorded rather than left implicit: a
    /// capture's provenance must not imply the camera supplied timing it did
    /// not. Normally 1 — the first packet of an RTSP stream, which carries the
    /// parameter sets and the first keyframe before any RTP timestamp has been
    /// established.
    std::int64_t timestampsSynthesised = 0;
    /// SHA-256 of the finished file. Empty until the segment is closed, because
    /// until then there is no file to hash — stating it earlier would be stating
    /// a digest of something still being written.
    std::string sha256;
    bool complete = false;
};

/// What a capture produced and what went wrong during it.
struct CaptureOutcome {
    std::vector<CaptureSegment> segments;
    std::vector<CaptureGap> gaps;
    /// Wall-clock span from the first packet to the last, including any gaps.
    /// Deliberately separate from the sum of segment durations, because the two
    /// differ exactly when the link dropped and an operator needs to see both.
    std::int64_t wallClockMs = 0;
    bool stoppedByOperator = false;
    /// Set when the capture ended because it could not continue.
    std::string failureReason;

    Microseconds recordedDurationUs() const {
        Microseconds total = 0;
        for (const auto& segment : segments) total += segment.durationUs;
        return total;
    }
    bool continuous() const { return gaps.empty(); }
};

struct CaptureProgress {
    int segmentIndex = 0;
    Microseconds segmentDurationUs = 0;
    std::int64_t bytesWritten = 0;
    std::int64_t framesWritten = 0;
    int gapsSoFar = 0;
    bool connected = false;
};

/// Returning false stops the capture. Called from the capture thread.
using CaptureProgressCallback = std::function<bool(const CaptureProgress&)>;

struct CaptureSettings {
    /// Stop after this long. Zero means run until stopped.
    std::int64_t maximumDurationMs = 0;
    /// Start a new segment at this size. Zero means one segment per connection.
    /// Segmenting bounds the damage from a truncated write and keeps individual
    /// exhibits a manageable size; it is not a gap and is not recorded as one.
    std::int64_t segmentBytes = 0;
    /// How long to keep retrying a dropped network camera before giving up.
    std::int64_t reconnectWindowMs = 30'000;
    /// Wait between reconnection attempts.
    std::int64_t reconnectDelayMs = 1'000;
    /// Seconds FFmpeg waits for the stream to open or for data to arrive. A
    /// camera that has gone away must not hang the capture thread forever.
    int openTimeoutSeconds = 10;
};

/// Opens a camera and writes what it sends into files.
///
/// The stream is **remuxed, not re-encoded**: packets are copied from the
/// camera's own bitstream into a container without being decoded. That keeps
/// the recording exactly what the camera produced — re-encoding would make
/// every frame a lossy derivative of the evidence, and no compression setting
/// makes that acceptable for a recording that may be examined frame by frame.
class CaptureSession {
public:
    CaptureSession();
    ~CaptureSession();
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    /// Records `camera` into `directory`, returning when the capture stops.
    ///
    /// Blocks. Callers that need a responsive interface run it on a worker and
    /// stop it with `requestStop()` from another thread.
    ///
    /// Fails immediately, before creating any file, when the transport cannot
    /// carry video — a Bluetooth control link is the case that matters.
    Result<CaptureOutcome> record(const CameraSource& camera,
                                  const std::filesystem::path& directory,
                                  const std::string& filenamePrefix,
                                  const CaptureSettings& settings = {},
                                  const CaptureProgressCallback& progress = {});

    /// Asks the capture to finish the current segment and return. Safe from any
    /// thread. The segment is closed properly and hashed; stopping does not
    /// discard what was recorded.
    void requestStop();

    bool stopRequested() const { return stopRequested_.load(std::memory_order_relaxed); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> stopRequested_{false};
};

/// Whether a live source can be reached at all, without recording anything.
///
/// Used by the interface to tell "wrong address" from "camera is not answering"
/// before an operator starts a capture they think is running.
Status probeCamera(const CameraSource& camera, int timeoutSeconds = 5);

}  // namespace trace
