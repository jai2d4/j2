#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/common/result.h"
#include "core/common/time_utils.h"
#include "core/models/derived_asset.h"
#include "core/models/evidence.h"
#include "core/services/derived_asset_service.h"
#include "core/storage/storage_layout.h"

namespace trace {

struct ClipExportRequest {
    std::string caseId;
    std::string caseNumber;
    Evidence evidence;
    Microseconds requestedStartUs = 0;
    Microseconds requestedEndUs = 0;
    std::string notes;
};

/// What extracting a clip actually produced.
///
/// `actualStartUs` is recorded separately from the request because a stream copy
/// cannot begin mid-frame: extraction starts at the nearest preceding keyframe, and a
/// clip that quietly claimed to begin somewhere it did not would misrepresent when the
/// footage starts.
struct ClipExportOutcome {
    DerivedAsset asset;
    Microseconds requestedStartUs = 0;
    Microseconds requestedEndUs = 0;
    Microseconds actualStartUs = 0;
    Microseconds actualEndUs = 0;
    bool startedOnKeyFrame = true;
    bool reencoded = false;
    std::int64_t packetsWritten = 0;
};

/// Extracts a time range from a managed original into a standalone file.
///
/// The extraction is a **stream copy**: the encoded packets are remuxed into a new
/// container without being decoded and re-encoded, so the clip contains the original
/// frames rather than a generation-loss copy of them. The source is opened read-only
/// and is never modified.
///
/// Re-encoding is deliberately not implemented in this phase. A range that cannot be
/// stream-copied is reported as such rather than silently transcoded, because a clip
/// produced by a different method is a different kind of artefact and the operator
/// should know which they have.
class ClipExportService {
public:
    ClipExportService(StorageLayout layout, std::shared_ptr<DerivedAssetService> derivedAssets);

    Result<ClipExportOutcome> exportClip(const ClipExportRequest& request);

private:
    StorageLayout layout_;
    std::shared_ptr<DerivedAssetService> derivedAssets_;
};

}  // namespace trace
