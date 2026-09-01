#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/models/evidence.h"
#include "core/services/audit_service.h"
#include "core/services/derived_asset_service.h"
#include "core/services/evidence_service.h"
#include "core/storage/storage_layout.h"
#include "media/capture/camera.h"
#include "media/capture/capture_session.h"

namespace trace {

/// Turns a live camera into evidence, with provenance that says what it is.
///
/// ## Why this is not EvidenceService::ingest with a different source
///
/// Ingestion has one story to tell: a file existed, TRACE copied it, and the
/// digest is what an outside party checks the copy against. Every word of that
/// is false for a capture. There was no file. TRACE made it. The digest attests
/// only that nothing has changed since TRACE stopped writing, and there is
/// nothing earlier to compare it to.
///
/// Running a capture through `ingest` alone would file it under that first story
/// — `ingestedAt`, `ingestedBy`, a `sourcePath` pointing at a staging file TRACE
/// itself wrote a moment earlier — and a reader of the record would reasonably
/// conclude the material came from somewhere. So the copy still goes through
/// `ingest`, because the copy-hash-verify path is the right one and duplicating
/// it would be worse, and then this writes the capture provenance on top: the
/// camera, the transport, the machine clock, the software and library versions,
/// every gap in the recording, and an explicit statement that no original
/// exists.
///
/// ## Every segment is its own exhibit
///
/// A capture that drops and reconnects produces several files, and they are
/// registered as several evidence items rather than being joined. Joining them
/// would produce one exhibit whose timeline is a lie about elapsed time. Each
/// item carries the gap that preceded it.
struct CaptureRegistration {
    std::string caseId;
    std::string caseNumber;
    CameraSource camera;
    /// Free text from the operator: what is being recorded and why.
    std::string description;
    CaptureSettings settings;
};

/// One captured segment, after it has been filed.
struct CapturedEvidence {
    Evidence evidence;
    CaptureSegment segment;
    /// The gap that came immediately before this segment, when there was one.
    /// Present on every segment after a reconnection.
    std::optional<CaptureGap> precedingGap;
};

struct CaptureRegistrationOutcome {
    std::vector<CapturedEvidence> items;
    CaptureOutcome capture;
    /// Segments that were recorded but could not be filed as evidence. The files
    /// are left in the staging directory rather than deleted: a recording that
    /// failed to register is still a recording, and discarding it to keep the
    /// return value tidy would be destroying the only copy.
    std::vector<std::pair<std::filesystem::path, std::string>> unregistered;

    bool anyRecorded() const { return !items.empty() || !unregistered.empty(); }
};

class CaptureService {
public:
    /// `evidence` is borrowed, not owned: the application holds one
    /// EvidenceService for its lifetime and every service that files something
    /// uses that one. It must outlive this.
    CaptureService(StorageLayout layout, EvidenceService& evidence,
                   std::shared_ptr<DerivedAssetService> derivedAssets,
                   std::shared_ptr<AuditService> audit);

    /// Records the camera and files what it produced.
    ///
    /// Blocks for the length of the capture. `stop()` ends it from another
    /// thread; everything recorded up to that point is still filed.
    Result<CaptureRegistrationOutcome> capture(const CaptureRegistration& request,
                                               const CaptureProgressCallback& progress = {});

    /// Ends the running capture cleanly. Safe from any thread.
    void stop();

    /// Whether a capture is running right now. Used by the interface to keep an
    /// operator from starting a second one over the top of the first.
    bool running() const { return running_.load(std::memory_order_relaxed); }

private:
    StorageLayout layout_;
    EvidenceService* evidence_ = nullptr;
    std::shared_ptr<DerivedAssetService> derivedAssets_;
    std::shared_ptr<AuditService> audit_;
    CaptureSession session_;
    std::atomic<bool> running_{false};
};

/// The capture provenance record, as JSON, for one segment.
///
/// Built here rather than inline so the same fields reach the database, the
/// audit trail and a report without three chances to disagree. Exposed for the
/// tests, which assert on the fields an examiner would be asked about.
JsonValue captureProvenance(const CameraSource& camera, const CaptureSegment& segment,
                            const CaptureOutcome& outcome,
                            const std::optional<CaptureGap>& precedingGap);

}  // namespace trace
