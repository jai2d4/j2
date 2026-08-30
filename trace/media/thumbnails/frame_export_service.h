#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/common/result.h"
#include "core/security/crypto.h"
#include "core/models/derived_asset.h"
#include "core/models/evidence.h"
#include "core/services/derived_asset_service.h"
#include "core/storage/storage_layout.h"
#include "media/ffmpeg/video_decoder.h"

namespace trace {

/// Turns decoded frames into derived assets.
///
/// Every exported frame is written into the case's own storage, hashed, and
/// filed with the operation, parameters, source timestamp and software versions
/// that produced it. The original evidence file is opened read-only and is never
/// modified by an export.
class FrameExportService {
public:
    FrameExportService(StorageLayout layout, std::shared_ptr<DerivedAssetService> derivedAssets);

    struct ExportRequest {
        std::string caseId;
        std::string caseNumber;
        Evidence evidence;
        const VideoFrameData* frame = nullptr;
        DecoderStreamInfo decoderInfo;
        std::string notes;
        /// Needed only when the managed original is an encrypted container.
        const crypto::SecretKey* key = nullptr;
    };

    /// Saves the frame currently on screen as a full-resolution PNG.
    Result<DerivedAsset> exportFrame(const ExportRequest& request);

    /// Generates (once) a preview image for the evidence browser and registers
    /// it as a derived asset. Returns the existing asset if one is already on
    /// disk.
    Result<DerivedAsset> ensureThumbnail(const std::string& caseId, const std::string& caseNumber,
                                         const Evidence& evidence, int width = 320,
                                         const crypto::SecretKey* key = nullptr);

private:
    StorageLayout layout_;
    std::shared_ptr<DerivedAssetService> derivedAssets_;
};

}  // namespace trace
