#include "media/capture/capture_service.h"

#include <utility>

#include "core/common/logging.h"
#include "core/common/time_utils.h"
#include "media/ffmpeg/ffmpeg_support.h"
#include "trace/trace_version.h"

namespace trace {
namespace {

constexpr const char* kComponent = "capture";
constexpr const char* kOperationType = "camera_capture";

/// The sentence that keeps a captured recording from being read as an import.
///
/// It is stored on the operation and in the audit trail rather than left to the
/// interface, because the record outlives the interface: a case exported and
/// opened elsewhere in five years still carries it.
constexpr const char* kNoOriginalNotice =
    "Recorded live by TRACE. There is no earlier original: this file is the first "
    "generation of this material, and its SHA-256 attests only that it has not "
    "changed since TRACE finished writing it.";

}  // namespace

JsonValue captureProvenance(const CameraSource& camera, const CaptureSegment& segment,
                            const CaptureOutcome& outcome,
                            const std::optional<CaptureGap>& precedingGap) {
    JsonValue provenance =
        JsonValue::object()
            .set("camera_id", camera.id)
            .set("camera_name", camera.name)
            // The redacted form, always. A capture record is read by people who
            // are not the operator, and an RTSP password in it would be
            // published with the case.
            .set("camera_address", camera.redactedUri())
            .set("transport", toString(camera.transport))
            .set("link", toString(camera.link))
            .set("segment_started_at", segment.startedAt)
            .set("segment_ended_at", segment.endedAt)
            .set("segment_duration_us", segment.durationUs)
            .set("frames_written", segment.framesWritten)
            .set("bytes_written", segment.bytesWritten)
            .set("sha256", segment.sha256)
            // Named so nothing downstream has to infer it from the absence of a
            // source path.
            .set("no_original_exists", true)
            .set("notice", kNoOriginalNotice)
            .set("recorded_by_host", AuditService::hostName())
            .set("clock", "system clock of the recording machine, UTC")
            .set("software", std::string(kApplicationName) + " " + kApplicationVersion);

    if (!camera.manufacturer.empty()) provenance.set("camera_manufacturer", camera.manufacturer);
    if (!camera.model.empty()) provenance.set("camera_model", camera.model);

    // Whether the capture as a whole was continuous belongs on every segment of
    // it: an examiner holding one exhibit must be able to see that the recording
    // it came from had gaps, without having to find the others.
    provenance.set("capture_continuous", outcome.continuous())
        .set("capture_gap_count", static_cast<std::int64_t>(outcome.gaps.size()))
        .set("capture_segment_count", static_cast<std::int64_t>(outcome.segments.size()))
        .set("capture_wall_clock_ms", outcome.wallClockMs)
        .set("capture_recorded_us", outcome.recordedDurationUs());

    if (precedingGap) {
        provenance.set("preceded_by_gap",
                       JsonValue::object()
                           .set("at_us", precedingGap->atUs)
                           .set("duration_ms", precedingGap->durationMs)
                           .set("reason", precedingGap->reason));
    }
    if (!outcome.failureReason.empty()) provenance.set("capture_ended_because", outcome.failureReason);
    return provenance;
}

CaptureService::CaptureService(StorageLayout layout, EvidenceService& evidence,
                               std::shared_ptr<DerivedAssetService> derivedAssets,
                               std::shared_ptr<AuditService> audit)
    : layout_(std::move(layout)),
      evidence_(&evidence),
      derivedAssets_(std::move(derivedAssets)),
      audit_(std::move(audit)) {}

void CaptureService::stop() { session_.requestStop(); }

Result<CaptureRegistrationOutcome> CaptureService::capture(
    const CaptureRegistration& request, const CaptureProgressCallback& progress) {
    using ResultType = Result<CaptureRegistrationOutcome>;

    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return ResultType::failure(ErrorCode::AlreadyExists, "A capture is already running",
                                   "Stop the running capture before starting another.");
    }
    struct RunningGuard {
        std::atomic<bool>& flag;
        ~RunningGuard() { flag.store(false, std::memory_order_release); }
    } runningGuard{running_};

    if (request.caseId.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "A capture must belong to a case");
    }
    if (!carriesVideo(request.camera.transport)) {
        // Refused here as well as inside CaptureSession, so the audit trail
        // records the attempt against the case rather than the refusal
        // happening silently one layer down.
        AuditRecord refusal;
        refusal.action = AuditAction::CaptureFailed;
        refusal.caseId = request.caseId;
        refusal.caseNumber = request.caseNumber;
        refusal.outcome = "failure";
        refusal.description = "Capture refused: a Bluetooth link cannot carry video";
        refusal.details = JsonValue::object()
                              .set("camera", request.camera.redactedUri())
                              .set("transport", toString(request.camera.transport));
        (void)audit_->record(refusal);
        return ResultType::failure(
            ErrorCode::Unsupported, "A Bluetooth link cannot carry video",
            "Use the Bluetooth link to have the camera start streaming, then capture from the "
            "network address it provides.");
    }

    if (auto status = layout_.ensureCaseDirectories(request.caseId); !status) {
        return ResultType(status.error());
    }
    const std::filesystem::path staging = layout_.captureStagingDirectory(request.caseId);

    const std::string startedAt = nowIso8601Utc();
    // The prefix carries the start time so a staging directory left behind by a
    // crash is still readable by a person.
    std::string prefix = "capture_" + startedAt;
    for (char& c : prefix) {
        if (c == ':' || c == '-' || c == '.') c = '_';
    }

    AuditRecord started;
    started.action = AuditAction::CaptureStarted;
    started.caseId = request.caseId;
    started.caseNumber = request.caseNumber;
    started.description = "Live capture started from " + request.camera.redactedUri();
    started.details = JsonValue::object()
                          .set("camera_id", request.camera.id)
                          .set("camera", request.camera.redactedUri())
                          .set("transport", toString(request.camera.transport))
                          .set("link", toString(request.camera.link))
                          .set("maximum_duration_ms", request.settings.maximumDurationMs)
                          .set("segment_bytes", request.settings.segmentBytes);
    if (auto audited = audit_->record(started); !audited) return ResultType(audited.error());

    auto recorded = session_.record(request.camera, staging, prefix, request.settings, progress);
    if (!recorded) {
        AuditRecord failed;
        failed.action = AuditAction::CaptureFailed;
        failed.caseId = request.caseId;
        failed.caseNumber = request.caseNumber;
        failed.outcome = "failure";
        failed.description = "Live capture failed: " + recorded.error().message();
        failed.details = JsonValue::object()
                             .set("camera", request.camera.redactedUri())
                             .set("error", recorded.error().toString());
        (void)audit_->record(failed);
        return ResultType(recorded.error());
    }

    CaptureRegistrationOutcome outcome;
    outcome.capture = recorded.take();

    // Which gap, if any, comes immediately before each segment. Gaps are
    // recorded with the running total of recorded time at the moment the link
    // dropped, so the gap that precedes segment N is the one whose position
    // equals the recorded duration of segments 0..N-1.
    //
    // Several can share that position: a camera that comes back, sends nothing,
    // and drops again produces one gap per attempt, and no segment between them.
    // They are summed rather than reported one at a time, because what precedes
    // the next segment is the whole absence, and quoting only the first would
    // understate how much time is missing.
    Microseconds elapsed = 0;
    std::vector<std::optional<CaptureGap>> preceding(outcome.capture.segments.size());
    for (std::size_t i = 0; i < outcome.capture.segments.size(); ++i) {
        if (i > 0) {
            CaptureGap combined;
            int matches = 0;
            for (const auto& gap : outcome.capture.gaps) {
                if (gap.atUs != elapsed) continue;
                combined.atUs = gap.atUs;
                combined.durationMs += gap.durationMs;
                combined.reason = gap.reason;
                ++matches;
            }
            if (matches == 1) {
                preceding[i] = combined;
            } else if (matches > 1) {
                combined.reason = "the camera dropped " + std::to_string(matches) +
                                  " times before video resumed";
                preceding[i] = combined;
            }
        }
        elapsed += outcome.capture.segments[i].durationUs;
    }

    for (std::size_t i = 0; i < outcome.capture.segments.size(); ++i) {
        const CaptureSegment& segment = outcome.capture.segments[i];

        IngestRequest ingest;
        ingest.caseId = request.caseId;
        ingest.sourcePath = segment.path;
        ingest.description =
            request.description.empty()
                ? "Captured live from " + request.camera.redactedUri()
                : request.description + " — captured live from " + request.camera.redactedUri();
        // Two segments of a live capture are never the same bytes, and refusing
        // one as a duplicate would discard a recording.
        ingest.allowDuplicate = true;

        auto filed = evidence_->ingest(ingest);
        if (!filed) {
            // Kept on disk. The staged file is at this moment the only copy of
            // material that was genuinely recorded, and deleting it because the
            // database write failed would destroy evidence to tidy up an error.
            outcome.unregistered.emplace_back(segment.path, filed.error().toString());
            logError(kComponent, "Captured segment could not be registered as evidence",
                     JsonValue::object()
                         .set("path", segment.path.string())
                         .set("detail", filed.error().toString())
                         .set("kept", true));
            continue;
        }

        CapturedEvidence item;
        item.evidence = filed.take().evidence;
        item.segment = segment;
        item.precedingGap = preceding[i];

        const JsonValue provenance =
            captureProvenance(request.camera, segment, outcome.capture, item.precedingGap);

        if (auto operation = derivedAssets_->recordOperation(
                request.caseId, item.evidence.id, kOperationType, provenance,
                ffmpegLibraryVersions(), kNoOriginalNotice, segment.startedAt, segment.endedAt);
            !operation) {
            // The evidence row exists and the file is filed; only the capture
            // provenance failed. Reported rather than rolled back — deleting a
            // recording because its provenance row would not write is the wrong
            // trade — and logged loudly, because an exhibit without its capture
            // record is exactly the thing this service exists to prevent.
            logError(kComponent, "Capture provenance could not be written",
                     JsonValue::object()
                         .set("evidence_id", item.evidence.id)
                         .set("evidence_number", item.evidence.evidenceNumber)
                         .set("detail", operation.error().toString()));
        }

        // The staged copy goes only once the managed copy has been written,
        // re-read and proved to match — which is what ingest does before it
        // returns. Keeping it would leave a second copy of the recording outside
        // managed storage, and in an encrypted workspace that copy would be in
        // the clear.
        std::error_code ec;
        std::filesystem::remove(segment.path, ec);

        AuditRecord filedRecord;
        filedRecord.action = AuditAction::CaptureCompleted;
        filedRecord.caseId = request.caseId;
        filedRecord.caseNumber = request.caseNumber;
        filedRecord.evidenceId = item.evidence.id;
        filedRecord.evidenceNumber = item.evidence.evidenceNumber;
        filedRecord.description =
            "Live capture filed as " + item.evidence.evidenceNumber + " from " +
            request.camera.redactedUri();
        filedRecord.details = provenance;
        if (auto audited = audit_->record(filedRecord); !audited) {
            logError(kComponent, "Capture audit record could not be written",
                     JsonValue::object()
                         .set("evidence_id", item.evidence.id)
                         .set("detail", audited.error().toString()));
        }

        outcome.items.push_back(std::move(item));
    }

    // A capture with a gap in it is recorded as interrupted whether or not it
    // eventually finished, because "completed" alongside a missing ninety
    // seconds is the misleading half of the truth.
    if (!outcome.capture.gaps.empty() || !outcome.capture.failureReason.empty()) {
        AuditRecord interrupted;
        interrupted.action = AuditAction::CaptureInterrupted;
        interrupted.caseId = request.caseId;
        interrupted.caseNumber = request.caseNumber;
        interrupted.outcome = "warning";
        interrupted.description =
            "Live capture was interrupted: " +
            std::to_string(outcome.capture.gaps.size()) + " gap(s) in the recording";

        JsonValue gaps = JsonValue::array();
        for (const auto& gap : outcome.capture.gaps) {
            gaps.push(JsonValue::object()
                            .set("at_us", gap.atUs)
                            .set("duration_ms", gap.durationMs)
                            .set("reason", gap.reason));
        }
        interrupted.details = JsonValue::object()
                                  .set("camera", request.camera.redactedUri())
                                  .set("gaps", gaps)
                                  .set("segments", static_cast<std::int64_t>(
                                                       outcome.capture.segments.size()))
                                  .set("recorded_us", outcome.capture.recordedDurationUs())
                                  .set("wall_clock_ms", outcome.capture.wallClockMs);
        if (!outcome.capture.failureReason.empty()) {
            interrupted.details.set("ended_because", outcome.capture.failureReason);
        }
        (void)audit_->record(interrupted);
    }

    // The staging directory is removed only when it is empty — that is, when
    // every segment was filed. One left behind is a signal, not litter.
    std::error_code ec;
    if (std::filesystem::is_empty(staging, ec) && !ec) {
        std::filesystem::remove(staging, ec);
    }

    logInfo(kComponent, "Capture filed",
            JsonValue::object()
                .set("case_id", request.caseId)
                .set("filed", static_cast<std::int64_t>(outcome.items.size()))
                .set("unregistered", static_cast<std::int64_t>(outcome.unregistered.size()))
                .set("gaps", static_cast<std::int64_t>(outcome.capture.gaps.size())));

    return ResultType::success(std::move(outcome));
}

}  // namespace trace
