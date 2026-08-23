#include "media/export/clip_export_service.h"

#include <system_error>
#include <vector>

#include "core/common/logging.h"
#include "core/common/uuid.h"
#include "media/ffmpeg/ffmpeg_support.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace trace {
namespace {

constexpr const char* kComponent = "clip";
constexpr AVRational kMicroseconds{1, 1'000'000};

Microseconds toMicroseconds(std::int64_t timestamp, AVRational timeBase) {
    if (timestamp == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    return av_rescale_q(timestamp, timeBase, kMicroseconds);
}

std::int64_t fromMicroseconds(Microseconds us, AVRational timeBase) {
    return av_rescale_q_rnd(us, kMicroseconds, timeBase,
                            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}

std::string timecodeForFilename(Microseconds positionUs) {
    std::string text = formatTimecode(positionUs);
    for (char& c : text) {
        if (c == ':' || c == '.') c = '-';
    }
    return text;
}

/// Closes FFmpeg handles however the function exits.
struct RemuxContexts {
    AVFormatContext* input = nullptr;
    AVFormatContext* output = nullptr;

    ~RemuxContexts() {
        if (output != nullptr) {
            if (output->pb != nullptr && (output->oformat->flags & AVFMT_NOFILE) == 0) {
                avio_closep(&output->pb);
            }
            avformat_free_context(output);
        }
        if (input != nullptr) avformat_close_input(&input);
    }
};

}  // namespace

ClipExportService::ClipExportService(StorageLayout layout,
                                     std::shared_ptr<DerivedAssetService> derivedAssets)
    : layout_(std::move(layout)), derivedAssets_(std::move(derivedAssets)) {}

Result<ClipExportOutcome> ClipExportService::exportClip(const ClipExportRequest& request) {
    using ResultType = Result<ClipExportOutcome>;
    initialiseFFmpeg();

    if (request.requestedEndUs <= request.requestedStartUs) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "A clip must end after it begins");
    }

    const auto source = layout_.resolve(request.evidence.storageRelPath);
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        return ResultType::failure(ErrorCode::NotFound,
                                   "The managed original is missing: " + source.string());
    }

    const std::string assetId = generateUuid();
    // Keep the source container: a stream copy cannot change the codec, and putting
    // the same packets in a foreign container is a good way to produce a file that
    // will not play.
    const std::string extension =
        source.has_extension() ? source.extension().string() : std::string(".mp4");
    const std::string filename = StorageLayout::derivedFilename(
        request.evidence.evidenceNumber, "clip", timecodeForFilename(request.requestedStartUs),
        extension, assetId);
    const auto destination = layout_.exportsDirectory(request.caseId) / filename;
    std::filesystem::create_directories(destination.parent_path(), ec);

    RemuxContexts contexts;
    ClipExportOutcome outcome;
    outcome.requestedStartUs = request.requestedStartUs;
    outcome.requestedEndUs = request.requestedEndUs;

    // ------------------------------------------------------------- input
    int rc = avformat_open_input(&contexts.input, source.string().c_str(), nullptr, nullptr);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not open the managed original: " + ffmpegErrorString(rc));
    }
    rc = avformat_find_stream_info(contexts.input, nullptr);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not read stream information: " + ffmpegErrorString(rc));
    }

    // ------------------------------------------------------------ output
    rc = avformat_alloc_output_context2(&contexts.output, nullptr, nullptr,
                                        destination.string().c_str());
    if (rc < 0 || contexts.output == nullptr) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not create the clip container: " + ffmpegErrorString(rc));
    }

    // Map the audio and video streams across, copying codec parameters verbatim.
    std::vector<int> outputIndex(contexts.input->nb_streams, -1);
    int videoStream = -1;
    for (unsigned int i = 0; i < contexts.input->nb_streams; ++i) {
        const AVCodecParameters* in = contexts.input->streams[i]->codecpar;
        if (in->codec_type != AVMEDIA_TYPE_VIDEO && in->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        AVStream* out = avformat_new_stream(contexts.output, nullptr);
        if (out == nullptr) {
            return ResultType::failure(ErrorCode::MediaError, "Could not add an output stream");
        }
        rc = avcodec_parameters_copy(out->codecpar, in);
        if (rc < 0) {
            return ResultType::failure(ErrorCode::MediaError,
                                       "Could not copy codec parameters: " + ffmpegErrorString(rc));
        }
        out->codecpar->codec_tag = 0;
        out->time_base = contexts.input->streams[i]->time_base;
        outputIndex[i] = out->index;
        if (in->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = static_cast<int>(i);
        }
    }
    if (videoStream < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "This item has no video stream to extract a clip from");
    }

    if ((contexts.output->oformat->flags & AVFMT_NOFILE) == 0) {
        rc = avio_open(&contexts.output->pb, destination.string().c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            return ResultType::failure(ErrorCode::IoError,
                                       "Could not write the clip: " + ffmpegErrorString(rc));
        }
    }
    rc = avformat_write_header(contexts.output, nullptr);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not write the clip header: " + ffmpegErrorString(rc));
    }

    // ----------------------------------------------------------- extract
    const AVRational videoTimeBase = contexts.input->streams[videoStream]->time_base;
    rc = av_seek_frame(contexts.input, videoStream,
                       fromMicroseconds(request.requestedStartUs, videoTimeBase),
                       AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not seek to the start of the range: " +
                                       ffmpegErrorString(rc));
    }

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        return ResultType::failure(ErrorCode::Internal, "Out of memory allocating a packet");
    }

    // One origin for every stream, taken from the first video packet written.
    // A per-stream origin would shift audio and video by different amounts and
    // desynchronise them — the clip would look right and sound late.
    Microseconds originUs = AV_NOPTS_VALUE;
    bool endReached = false;
    Status writeError = Status::success();

    while (!endReached && av_read_frame(contexts.input, packet) >= 0) {
        const int stream = packet->stream_index;
        if (stream >= static_cast<int>(outputIndex.size()) || outputIndex[stream] < 0) {
            av_packet_unref(packet);
            continue;
        }

        const AVRational inTimeBase = contexts.input->streams[stream]->time_base;
        const Microseconds ptsUs = toMicroseconds(packet->pts, inTimeBase);

        if (stream == videoStream) {
            if (originUs == AV_NOPTS_VALUE) {
                // Where the extraction genuinely begins: the keyframe at or before
                // the requested moment.
                originUs = ptsUs == AV_NOPTS_VALUE ? request.requestedStartUs : ptsUs;
                outcome.actualStartUs = originUs;
                outcome.startedOnKeyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            }
            if (ptsUs != AV_NOPTS_VALUE && ptsUs > request.requestedEndUs) {
                endReached = true;
                av_packet_unref(packet);
                break;
            }
            if (ptsUs != AV_NOPTS_VALUE) outcome.actualEndUs = ptsUs;
        }

        // Audio that precedes the video keyframe would land before the clip starts.
        if (originUs == AV_NOPTS_VALUE || (ptsUs != AV_NOPTS_VALUE && ptsUs < originUs)) {
            av_packet_unref(packet);
            continue;
        }

        AVStream* out = contexts.output->streams[outputIndex[stream]];
        const std::int64_t originInStream = fromMicroseconds(originUs, inTimeBase);
        if (packet->pts != AV_NOPTS_VALUE) {
            packet->pts = av_rescale_q_rnd(
                packet->pts - originInStream, inTimeBase, out->time_base,
                static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        }
        if (packet->dts != AV_NOPTS_VALUE) {
            packet->dts = av_rescale_q_rnd(
                packet->dts - originInStream, inTimeBase, out->time_base,
                static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        }
        packet->duration = av_rescale_q(packet->duration, inTimeBase, out->time_base);
        packet->pos = -1;
        packet->stream_index = out->index;

        rc = av_interleaved_write_frame(contexts.output, packet);
        av_packet_unref(packet);
        if (rc < 0) {
            writeError = Status::failure(ErrorCode::IoError,
                                         "Failed while writing the clip: " + ffmpegErrorString(rc));
            break;
        }
        ++outcome.packetsWritten;
    }
    av_packet_free(&packet);

    if (!writeError) {
        std::filesystem::remove(destination, ec);
        return ResultType(writeError.error());
    }

    rc = av_write_trailer(contexts.output);
    if (rc < 0) {
        std::filesystem::remove(destination, ec);
        return ResultType::failure(ErrorCode::MediaError,
                                   "Could not finalise the clip: " + ffmpegErrorString(rc));
    }
    if (outcome.packetsWritten == 0) {
        std::filesystem::remove(destination, ec);
        return ResultType::failure(ErrorCode::MediaError,
                                   "No frames fell inside the requested range");
    }

    // ------------------------------------------------- register the asset
    DerivedAssetRegistration registration;
    registration.caseId = request.caseId;
    registration.caseNumber = request.caseNumber;
    registration.evidenceId = request.evidence.id;
    registration.evidenceNumber = request.evidence.evidenceNumber;
    registration.type = DerivedAssetType::Clip;
    registration.file = destination;
    registration.operationType = "clip_extraction";
    registration.mediaType = request.evidence.mimeType;
    registration.sourceStartUs = outcome.actualStartUs;
    registration.sourceEndUs = outcome.actualEndUs;
    registration.notes = request.notes;
    registration.auditAction = AuditAction::ClipExported;
    registration.auditDescription = "Clip extracted from " + request.evidence.evidenceNumber +
                                    " covering " + formatTimecode(outcome.actualStartUs) + " to " +
                                    formatTimecode(outcome.actualEndUs);
    registration.libraryVersions = ffmpegLibraryVersions();
    registration.parameters =
        JsonValue::object()
            .set("method", "libavformat stream copy (remux); packets are not decoded or re-encoded")
            .set("reencoded", false)
            .set("requested_start_us", request.requestedStartUs)
            .set("requested_end_us", request.requestedEndUs)
            .set("actual_start_us", outcome.actualStartUs)
            .set("actual_end_us", outcome.actualEndUs)
            .set("requested_start_timecode", formatTimecode(request.requestedStartUs))
            .set("actual_start_timecode", formatTimecode(outcome.actualStartUs))
            .set("started_on_key_frame", outcome.startedOnKeyFrame)
            .set("packets_written", outcome.packetsWritten)
            .set("source_sha256", request.evidence.sha256);
    if (outcome.actualStartUs != request.requestedStartUs) {
        registration.parameters.set(
            "start_adjusted_because",
            "a stream copy must begin at a keyframe, so extraction started at the nearest "
            "preceding one");
    }

    auto registered = derivedAssets_->registerAsset(registration);
    if (!registered) {
        std::filesystem::remove(destination, ec);
        return ResultType(registered.error());
    }
    outcome.asset = registered.take();

    logInfo(kComponent, "Clip extracted",
            JsonValue::object()
                .set("evidence", request.evidence.evidenceNumber)
                .set("file", filename)
                .set("requested_start_us", request.requestedStartUs)
                .set("actual_start_us", outcome.actualStartUs)
                .set("packets", outcome.packetsWritten));

    return ResultType::success(std::move(outcome));
}

}  // namespace trace
