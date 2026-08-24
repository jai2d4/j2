#include "media/audio/waveform_service.h"

#include <fstream>
#include <system_error>

#include "core/common/uuid.h"
#include "media/ffmpeg/ffmpeg_support.h"

namespace trace {

WaveformService::WaveformService(StorageLayout layout,
                                 std::shared_ptr<DerivedAssetService> derivedAssets)
    : layout_(std::move(layout)), derivedAssets_(std::move(derivedAssets)) {}

Result<DerivedAsset> WaveformService::ensureWaveform(
    const std::string& caseId, const std::string& caseNumber, const Evidence& evidence,
    int buckets, const std::function<bool(double)>& progress) {
    using ResultType = Result<DerivedAsset>;

    // Already built, and the file is still where it was recorded.
    auto existing = derivedAssets_->listForEvidence(evidence.id);
    if (existing) {
        for (const auto& asset : existing.value()) {
            if (asset.type != DerivedAssetType::Waveform) continue;
            std::error_code ec;
            if (std::filesystem::exists(layout_.resolve(asset.storageRelPath), ec)) {
                return ResultType::success(asset);
            }
        }
    }

    const auto source = layout_.resolve(evidence.storageRelPath);
    auto built = WaveformBuilder::build(source, buckets, progress);
    if (!built) return ResultType(built.error());
    const Waveform waveform = built.take();

    const std::string assetId = generateUuid();
    const std::string filename =
        StorageLayout::derivedFilename(evidence.evidenceNumber, "waveform", "", ".json", assetId);
    const auto destination = layout_.workingDirectory(caseId) / filename;

    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    {
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out) {
            return ResultType::failure(ErrorCode::IoError,
                                       "Could not write the waveform to " + destination.string());
        }
        const std::string json = waveform.toJson();
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.close();
        if (!out) {
            return ResultType::failure(ErrorCode::IoError, "Failed while writing the waveform");
        }
    }

    DerivedAssetRegistration registration;
    registration.caseId = caseId;
    registration.caseNumber = caseNumber;
    registration.evidenceId = evidence.id;
    registration.evidenceNumber = evidence.evidenceNumber;
    registration.type = DerivedAssetType::Waveform;
    registration.file = destination;
    registration.operationType = "waveform_generation";
    registration.mediaType = "application/json";
    registration.sourceStartUs = 0;
    registration.sourceEndUs = waveform.durationUs;
    registration.auditAction = AuditAction::DerivedAssetCreated;
    registration.auditDescription =
        "Waveform generated for " + evidence.evidenceNumber;
    registration.libraryVersions = ffmpegLibraryVersions();
    registration.parameters =
        JsonValue::object()
            .set("method",
                 "libavcodec decode, libswresample to mono 8 kHz, peak and RMS per bucket")
            .set("buckets", static_cast<std::int64_t>(waveform.buckets()))
            .set("duration_us", waveform.durationUs)
            .set("source_sample_rate", static_cast<std::int64_t>(waveform.sourceSampleRate))
            .set("source_channels", static_cast<std::int64_t>(waveform.sourceChannels))
            .set("normalised", false)
            .set("note",
                 "Amplitudes are relative to full scale, not to the loudest point in this "
                 "recording: how loud the audio is is itself information about the evidence.");

    auto registered = derivedAssets_->registerAsset(registration);
    if (!registered) {
        std::filesystem::remove(destination, ec);
        return ResultType(registered.error());
    }
    return ResultType::success(registered.take());
}

Result<Waveform> WaveformService::load(const DerivedAsset& asset) const {
    using ResultType = Result<Waveform>;
    if (asset.type != DerivedAssetType::Waveform) {
        return ResultType::failure(ErrorCode::InvalidArgument, "That asset is not a waveform");
    }

    const auto path = layout_.resolve(asset.storageRelPath);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return ResultType::failure(ErrorCode::NotFound,
                                   "The waveform file is missing: " + path.string());
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Waveform::fromJson(text);
}

}  // namespace trace
