#include "core/repositories/metadata_repository.h"

#include <utility>

#include "core/common/uuid.h"

namespace trace {
namespace {

constexpr const char* kColumns =
    "evidence_id, container_format, format_long_name, duration_us, bit_rate, video_codec, "
    "video_codec_long, video_profile, width, height, pixel_format, display_aspect, "
    "avg_frame_rate, nominal_frame_rate, frame_rate_mode, video_time_base, video_bit_rate, "
    "frame_count, audio_codec, audio_codec_long, audio_channels, audio_channel_layout, "
    "audio_sample_rate, audio_sample_format, audio_bit_rate, video_stream_count, "
    "audio_stream_count, subtitle_stream_count, creation_time, gps_location, make, model, "
    "raw_metadata_json, extraction_status, extraction_error, extractor, extracted_at";

constexpr const char* kStreamColumns =
    "id, evidence_id, stream_index, stream_type, codec_name, codec_long_name, time_base, "
    "duration_us, bit_rate, width, height, pixel_format, frame_rate, sample_rate, channels, "
    "channel_layout, language, metadata_json";

MediaStreamInfo readStream(const Statement& stmt) {
    MediaStreamInfo info;
    info.id = stmt.columnText(0);
    info.evidenceId = stmt.columnText(1);
    info.index = stmt.columnInt(2);
    info.type = streamTypeFromString(stmt.columnText(3));
    info.codecName = stmt.columnOptionalText(4);
    info.codecLongName = stmt.columnOptionalText(5);
    info.timeBase = stmt.columnOptionalText(6);
    info.durationUs = stmt.columnOptionalInt64(7);
    info.bitRate = stmt.columnOptionalInt64(8);
    info.width = stmt.columnOptionalInt64(9);
    info.height = stmt.columnOptionalInt64(10);
    info.pixelFormat = stmt.columnOptionalText(11);
    info.frameRate = stmt.columnOptionalDouble(12);
    info.sampleRate = stmt.columnOptionalInt64(13);
    info.channels = stmt.columnOptionalInt64(14);
    info.channelLayout = stmt.columnOptionalText(15);
    info.language = stmt.columnOptionalText(16);
    info.metadataJson = stmt.columnText(17);
    return info;
}

}  // namespace

MetadataRepository::MetadataRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Status MetadataRepository::save(const MediaMetadata& metadata) {
    Transaction transaction(*database_);
    if (auto status = transaction.begin(); !status) return status;

    {
        auto prepared = database_->prepare(
            std::string("INSERT OR REPLACE INTO evidence_metadata (") + kColumns +
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
            "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
        if (!prepared) return Status(prepared.error());

        Statement stmt = prepared.take();
        stmt.bind(1, metadata.evidenceId)
            .bindOptional(2, metadata.containerFormat)
            .bindOptional(3, metadata.formatLongName)
            .bindOptional(4, metadata.durationUs)
            .bindOptional(5, metadata.bitRate)
            .bindOptional(6, metadata.videoCodec)
            .bindOptional(7, metadata.videoCodecLongName)
            .bindOptional(8, metadata.videoProfile)
            .bindOptional(9, metadata.width)
            .bindOptional(10, metadata.height)
            .bindOptional(11, metadata.pixelFormat)
            .bindOptional(12, metadata.displayAspect)
            .bindOptional(13, metadata.averageFrameRate)
            .bindOptional(14, metadata.nominalFrameRate)
            .bind(15, std::string(toString(metadata.frameRateMode)))
            .bindOptional(16, metadata.videoTimeBase)
            .bindOptional(17, metadata.videoBitRate)
            .bindOptional(18, metadata.frameCount)
            .bindOptional(19, metadata.audioCodec)
            .bindOptional(20, metadata.audioCodecLongName)
            .bindOptional(21, metadata.audioChannels)
            .bindOptional(22, metadata.audioChannelLayout)
            .bindOptional(23, metadata.audioSampleRate)
            .bindOptional(24, metadata.audioSampleFormat)
            .bindOptional(25, metadata.audioBitRate)
            .bind(26, metadata.videoStreamCount)
            .bind(27, metadata.audioStreamCount)
            .bind(28, metadata.subtitleStreamCount)
            .bindOptional(29, metadata.creationTime)
            .bindOptional(30, metadata.gpsLocation)
            .bindOptional(31, metadata.make)
            .bindOptional(32, metadata.model)
            .bind(33, metadata.rawMetadataJson)
            .bind(34, std::string(toString(metadata.extractionStatus)))
            .bindOptional(35, metadata.extractionError)
            .bind(36, metadata.extractor)
            .bind(37, metadata.extractedAt);
        if (auto status = stmt.run(); !status) return status;
    }

    {
        auto prepared = database_->prepare("DELETE FROM media_streams WHERE evidence_id = ?;");
        if (!prepared) return Status(prepared.error());
        Statement stmt = prepared.take();
        stmt.bind(1, metadata.evidenceId);
        if (auto status = stmt.run(); !status) return status;
    }

    for (const auto& stream : metadata.streams) {
        auto prepared = database_->prepare(
            std::string("INSERT INTO media_streams (") + kStreamColumns +
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
        if (!prepared) return Status(prepared.error());

        Statement stmt = prepared.take();
        stmt.bind(1, stream.id.empty() ? generateUuid() : stream.id)
            .bind(2, metadata.evidenceId)
            .bind(3, stream.index)
            .bind(4, std::string(toString(stream.type)))
            .bindOptional(5, stream.codecName)
            .bindOptional(6, stream.codecLongName)
            .bindOptional(7, stream.timeBase)
            .bindOptional(8, stream.durationUs)
            .bindOptional(9, stream.bitRate)
            .bindOptional(10, stream.width)
            .bindOptional(11, stream.height)
            .bindOptional(12, stream.pixelFormat)
            .bindOptional(13, stream.frameRate)
            .bindOptional(14, stream.sampleRate)
            .bindOptional(15, stream.channels)
            .bindOptional(16, stream.channelLayout)
            .bindOptional(17, stream.language)
            .bind(18, stream.metadataJson);
        if (auto status = stmt.run(); !status) return status;
    }

    return transaction.commit();
}

Result<std::optional<MediaMetadata>> MetadataRepository::findByEvidenceId(
    const std::string& evidenceId) {
    using ResultType = Result<std::optional<MediaMetadata>>;

    auto prepared = database_->prepare(std::string("SELECT ") + kColumns +
                                       " FROM evidence_metadata WHERE evidence_id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);

    MediaMetadata metadata;
    metadata.evidenceId = stmt.columnText(0);
    metadata.containerFormat = stmt.columnOptionalText(1);
    metadata.formatLongName = stmt.columnOptionalText(2);
    metadata.durationUs = stmt.columnOptionalInt64(3);
    metadata.bitRate = stmt.columnOptionalInt64(4);
    metadata.videoCodec = stmt.columnOptionalText(5);
    metadata.videoCodecLongName = stmt.columnOptionalText(6);
    metadata.videoProfile = stmt.columnOptionalText(7);
    metadata.width = stmt.columnOptionalInt64(8);
    metadata.height = stmt.columnOptionalInt64(9);
    metadata.pixelFormat = stmt.columnOptionalText(10);
    metadata.displayAspect = stmt.columnOptionalText(11);
    metadata.averageFrameRate = stmt.columnOptionalDouble(12);
    metadata.nominalFrameRate = stmt.columnOptionalDouble(13);
    metadata.frameRateMode = frameRateModeFromString(stmt.columnText(14));
    metadata.videoTimeBase = stmt.columnOptionalText(15);
    metadata.videoBitRate = stmt.columnOptionalInt64(16);
    metadata.frameCount = stmt.columnOptionalInt64(17);
    metadata.audioCodec = stmt.columnOptionalText(18);
    metadata.audioCodecLongName = stmt.columnOptionalText(19);
    metadata.audioChannels = stmt.columnOptionalInt64(20);
    metadata.audioChannelLayout = stmt.columnOptionalText(21);
    metadata.audioSampleRate = stmt.columnOptionalInt64(22);
    metadata.audioSampleFormat = stmt.columnOptionalText(23);
    metadata.audioBitRate = stmt.columnOptionalInt64(24);
    metadata.videoStreamCount = stmt.columnInt64(25);
    metadata.audioStreamCount = stmt.columnInt64(26);
    metadata.subtitleStreamCount = stmt.columnInt64(27);
    metadata.creationTime = stmt.columnOptionalText(28);
    metadata.gpsLocation = stmt.columnOptionalText(29);
    metadata.make = stmt.columnOptionalText(30);
    metadata.model = stmt.columnOptionalText(31);
    metadata.rawMetadataJson = stmt.columnText(32);
    metadata.extractionStatus = metadataExtractionStatusFromString(stmt.columnText(33));
    metadata.extractionError = stmt.columnOptionalText(34);
    metadata.extractor = stmt.columnText(35);
    metadata.extractedAt = stmt.columnText(36);

    auto streams = listStreams(evidenceId);
    if (!streams) return ResultType(streams.error());
    metadata.streams = streams.take();

    return ResultType::success(std::move(metadata));
}

Result<std::vector<MediaStreamInfo>> MetadataRepository::listStreams(const std::string& evidenceId) {
    using ResultType = Result<std::vector<MediaStreamInfo>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kStreamColumns +
                                       " FROM media_streams WHERE evidence_id = ? ORDER BY stream_index;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, evidenceId);

    std::vector<MediaStreamInfo> streams;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        streams.push_back(readStream(stmt));
    }
    return ResultType::success(std::move(streams));
}

}  // namespace trace
