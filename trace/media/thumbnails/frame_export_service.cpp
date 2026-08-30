#include "media/thumbnails/frame_export_service.h"

#include <utility>

#include "core/common/logging.h"
#include "core/common/string_utils.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "media/ffmpeg/ffmpeg_support.h"
#include "media/thumbnails/image_writer.h"

namespace trace {
namespace {

constexpr const char* kComponent = "export";

/// "00-00-05-000" — a timecode that is safe in a filename.
std::string timecodeForFilename(Microseconds positionUs) {
    std::string text = formatTimecode(positionUs);
    for (char& c : text) {
        if (c == ':' || c == '.') c = '-';
    }
    return text;
}

}  // namespace

FrameExportService::FrameExportService(StorageLayout layout,
                                       std::shared_ptr<DerivedAssetService> derivedAssets)
    : layout_(std::move(layout)), derivedAssets_(std::move(derivedAssets)) {}

Result<DerivedAsset> FrameExportService::exportFrame(const ExportRequest& request) {
    using ResultType = Result<DerivedAsset>;

    if (request.frame == nullptr || !request.frame->valid()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "There is no decoded frame to save yet");
    }

    const std::string assetId = generateUuid();
    const std::string filename = StorageLayout::derivedFilename(
        request.evidence.evidenceNumber, "frame", timecodeForFilename(request.frame->presentationUs),
        ".png", assetId);
    const auto destination = layout_.exportsDirectory(request.caseId) / filename;

    if (auto status = writeFramePng(*request.frame, destination); !status) {
        return ResultType(status.error());
    }

    DerivedAssetRegistration registration;
    registration.caseId = request.caseId;
    registration.caseNumber = request.caseNumber;
    registration.evidenceId = request.evidence.id;
    registration.evidenceNumber = request.evidence.evidenceNumber;
    registration.type = DerivedAssetType::FrameExport;
    registration.file = destination;
    registration.operationType = "frame_extraction";
    registration.mediaType = "image/png";
    registration.sourceStartUs = request.frame->presentationUs;
    registration.sourceFrameNumber = request.frame->frameNumber;
    registration.notes = request.notes;
    registration.auditAction = AuditAction::FrameExtracted;
    registration.auditDescription =
        "Frame extracted from " + request.evidence.evidenceNumber + " at " +
        formatTimecode(request.frame->presentationUs);
    registration.libraryVersions = ffmpegLibraryVersions();
    registration.parameters =
        JsonValue::object()
            .set("method", "libavcodec decode to RGB24, lossless PNG encode")
            .set("source_timestamp_us", request.frame->presentationUs)
            .set("source_timecode", formatTimecode(request.frame->presentationUs))
            .set("width", static_cast<std::int64_t>(request.frame->width))
            .set("height", static_cast<std::int64_t>(request.frame->height))
            .set("scaled", false)
            .set("source_codec", request.decoderInfo.codecName)
            .set("source_pixel_format", request.decoderInfo.pixelFormat)
            .set("frame_rate_mode", toString(request.decoderInfo.frameRateMode))
            // Which decoder produced these pixels. A hardware decoder is a
            // different implementation of the same standard, so an exhibit that
            // came through one has to say so — otherwise two frames exported
            // from the same recording on differently configured workstations
            // could differ with nothing in the record to explain it.
            .set("decoder", request.decoderInfo.hardwareDevice.empty()
                                ? std::string("software")
                                : "hardware:" + request.decoderInfo.hardwareDevice)
            .set("key_frame", request.frame->keyFrame);
    if (request.frame->frameNumber) {
        registration.parameters.set("source_frame_number", *request.frame->frameNumber);
    } else {
        registration.parameters.set("source_frame_number_available", false);
    }

    auto registered = derivedAssets_->registerAsset(registration);
    if (!registered) {
        std::error_code ec;
        std::filesystem::remove(destination, ec);
        return ResultType(registered.error());
    }

    logInfo(kComponent, "Frame exported",
            JsonValue::object()
                .set("evidence", request.evidence.evidenceNumber)
                .set("timecode", formatTimecode(request.frame->presentationUs))
                .set("file", filename));
    return registered;
}

Result<DerivedAsset> FrameExportService::ensureThumbnail(const std::string& caseId,
                                                         const std::string& caseNumber,
                                                         const Evidence& evidence, int width,
                                                         const crypto::SecretKey* key) {
    using ResultType = Result<DerivedAsset>;

    auto existing = derivedAssets_->listForEvidence(evidence.id);
    if (existing) {
        for (const auto& asset : existing.value()) {
            if (asset.type != DerivedAssetType::Thumbnail) continue;
            std::error_code ec;
            if (std::filesystem::exists(layout_.resolve(asset.storageRelPath), ec)) {
                return ResultType::success(asset);
            }
        }
    }

    const auto source = layout_.resolve(evidence.storageRelPath);
    auto decoder = VideoDecoder::open(source, key);
    if (!decoder) return ResultType(decoder.error());
    auto decoderPtr = decoder.take();

    // A frame a little way in avoids the black or colour-bar leader that many
    // recorders emit at the start of a clip.
    const Microseconds target =
        decoderPtr->info().durationUs > 2'000'000 ? decoderPtr->info().durationUs / 10 : 0;
    auto frame = decoderPtr->frameAt(target);
    if (!frame) {
        frame = decoderPtr->frameAt(0);
        if (!frame) return ResultType(frame.error());
    }
    const VideoFrameData frameData = frame.take();

    const std::string assetId = generateUuid();
    const std::string filename = StorageLayout::derivedFilename(
        evidence.evidenceNumber, "thumb", timecodeForFilename(frameData.presentationUs), ".png",
        assetId);
    const auto destination = layout_.thumbnailsDirectory(caseId) / filename;

    if (auto status = writeFramePng(frameData, destination, width); !status) {
        return ResultType(status.error());
    }

    DerivedAssetRegistration registration;
    registration.caseId = caseId;
    registration.caseNumber = caseNumber;
    registration.evidenceId = evidence.id;
    registration.evidenceNumber = evidence.evidenceNumber;
    registration.type = DerivedAssetType::Thumbnail;
    registration.file = destination;
    registration.operationType = "thumbnail_generation";
    registration.mediaType = "image/png";
    registration.sourceStartUs = frameData.presentationUs;
    registration.sourceFrameNumber = frameData.frameNumber;
    registration.auditDescription =
        "Preview image generated for " + evidence.evidenceNumber;
    registration.libraryVersions = ffmpegLibraryVersions();
    registration.parameters = JsonValue::object()
                                  .set("method", "libavcodec decode, bilinear downscale, PNG encode")
                                  .set("source_timestamp_us", frameData.presentationUs)
                                  .set("requested_width", static_cast<std::int64_t>(width))
                                  .set("source_width", static_cast<std::int64_t>(frameData.width))
                                  .set("source_height", static_cast<std::int64_t>(frameData.height))
                                  .set("scaled", true);

    auto registered = derivedAssets_->registerAsset(registration);
    if (!registered) {
        std::error_code ec;
        std::filesystem::remove(destination, ec);
        return ResultType(registered.error());
    }
    return registered;
}

}  // namespace trace
