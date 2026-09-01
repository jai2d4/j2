#include "media/capture/capture_session.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
}

#if defined(TRACE_WITH_AVDEVICE)
extern "C" {
#include <libavdevice/avdevice.h>
}
#endif

#include "core/common/logging.h"
#include "core/security/file_hasher.h"
#include "media/ffmpeg/ffmpeg_support.h"

namespace trace {
namespace {

constexpr const char* kComponent = "capture";

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// The input format a local device needs. Network sources are detected from the
/// URI by avformat and need none.
const AVInputFormat* localDeviceFormat() {
#if defined(TRACE_WITH_AVDEVICE)
#if defined(_WIN32)
    return av_find_input_format("dshow");
#elif defined(__APPLE__)
    return av_find_input_format("avfoundation");
#else
    return av_find_input_format("video4linux2");
#endif
#else
    return nullptr;
#endif
}

/// Options handed to avformat for a live source.
///
/// The timeouts are the important part: without them a camera that stops
/// answering mid-handshake blocks the capture thread indefinitely, and the
/// operator sees a capture that is neither recording nor failing.
void applyOpenOptions(AVDictionary** options, const CameraSource& camera, int timeoutSeconds) {
    const std::string micros = std::to_string(static_cast<std::int64_t>(timeoutSeconds) * 1'000'000);
    // avformat spells this differently per protocol; setting both is harmless
    // and covers RTSP/HTTP as well as the generic socket layer.
    av_dict_set(options, "timeout", micros.c_str(), 0);
    av_dict_set(options, "stimeout", micros.c_str(), 0);
    av_dict_set(options, "rw_timeout", micros.c_str(), 0);

    if (camera.transport == CameraTransport::NetworkStream) {
        // TCP rather than the default UDP for RTSP. A dropped UDP packet is a
        // corrupt frame that nothing downstream can distinguish from camera
        // noise; TCP turns the same event into a stall or a clean disconnect,
        // which is something TRACE can record honestly as a gap.
        av_dict_set(options, "rtsp_transport", "tcp", 0);
        // Bounded, so a camera that floods cannot grow the buffer without limit.
        av_dict_set(options, "buffer_size", "2097152", 0);
    }
}

std::string segmentName(const std::string& prefix, int index) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "_%03d.mkv", index);
    // Matroska rather than MP4: an MP4 whose moov atom was never written — which
    // is what a capture killed by a power cut produces — is unplayable, while a
    // truncated Matroska file plays up to the point it stopped. For a recording
    // that may be interrupted, that is the difference between partial evidence
    // and none.
    return prefix + buffer;
}

}  // namespace

struct CaptureSession::Impl {
    AVFormatContext* input = nullptr;
    AVFormatContext* output = nullptr;
    std::vector<int> streamMap;

    void closeInput() {
        if (input != nullptr) avformat_close_input(&input);
        streamMap.clear();
    }

    void closeOutput() {
        if (output == nullptr) return;
        if ((output->oformat->flags & AVFMT_NOFILE) == 0 && output->pb != nullptr) {
            avio_closep(&output->pb);
        }
        avformat_free_context(output);
        output = nullptr;
    }

    ~Impl() {
        closeOutput();
        closeInput();
    }
};

CaptureSession::CaptureSession() : impl_(std::make_unique<Impl>()) {}
CaptureSession::~CaptureSession() = default;

void CaptureSession::requestStop() { stopRequested_.store(true, std::memory_order_relaxed); }

namespace {

/// FFmpeg's way of asking "should I give up?" during a blocking call.
///
/// Without it, `requestStop()` is only observed between packets — so an operator
/// who presses Stop while TRACE is part-way through a ten-second connection
/// attempt waits out the whole timeout. Worse, a camera that has gone away
/// entirely makes Stop look broken. avformat polls this from inside
/// avformat_open_input and av_read_frame, which is the only place a stop can be
/// noticed while they are blocked.
int shouldAbort(void* opaque) {
    const auto* stopRequested = static_cast<const std::atomic<bool>*>(opaque);
    return (stopRequested != nullptr && stopRequested->load(std::memory_order_relaxed)) ? 1 : 0;
}

/// Opens the camera. Separated so the reconnect loop can call it repeatedly
/// without duplicating option handling.
Status openInput(AVFormatContext** input, const CameraSource& camera, int timeoutSeconds,
                 const std::atomic<bool>* stopRequested) {
    initialiseFFmpeg();
#if defined(TRACE_WITH_AVDEVICE)
    avdevice_register_all();
#endif

    if (camera.transport == CameraTransport::LocalDevice && localDeviceFormat() == nullptr) {
        return Status::failure(
            ErrorCode::Unsupported, "This build of TRACE cannot open an attached camera",
            "It was built without libavdevice, so USB cameras and capture cards are unavailable. "
            "Network cameras are unaffected.");
    }

    // The context is allocated here rather than by avformat_open_input, because
    // the interrupt callback has to be in place *before* the open blocks.
    *input = avformat_alloc_context();
    if (*input == nullptr) {
        return Status::failure(ErrorCode::Internal, "Unable to prepare the camera connection");
    }
    (*input)->interrupt_callback.callback = shouldAbort;
    (*input)->interrupt_callback.opaque = const_cast<std::atomic<bool>*>(stopRequested);

    AVDictionary* options = nullptr;
    applyOpenOptions(&options, camera, timeoutSeconds);

    const AVInputFormat* format =
        camera.transport == CameraTransport::LocalDevice ? localDeviceFormat() : nullptr;
    const int rc = avformat_open_input(input, camera.uri.c_str(), format, &options);
    av_dict_free(&options);

    if (rc < 0) {
        // avformat_open_input frees and nulls the context on failure, including
        // the one allocated above.
        if (shouldAbort(const_cast<std::atomic<bool>*>(stopRequested)) != 0) {
            return Status::failure(ErrorCode::Cancelled, "The capture was stopped while connecting");
        }
        return Status::failure(ErrorCode::MediaError,
                               "Could not open the camera: " + camera.redactedUri(),
                               ffmpegErrorString(rc));
    }
    if (const int probe = avformat_find_stream_info(*input, nullptr); probe < 0) {
        avformat_close_input(input);
        return Status::failure(ErrorCode::MediaError, "The camera opened but sent no usable streams",
                               ffmpegErrorString(probe));
    }
    return Status::success();
}

}  // namespace

Status probeCamera(const CameraSource& camera, int timeoutSeconds) {
    if (!carriesVideo(camera.transport)) {
        return Status::failure(
            ErrorCode::Unsupported, "A Bluetooth link cannot carry video",
            "Bluetooth is used to find and control a camera. Use it to have the camera start "
            "streaming, then capture from the network address it provides.");
    }
    AVFormatContext* input = nullptr;
    if (auto status = openInput(&input, camera, timeoutSeconds, nullptr); !status) return status;

    const int video = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    avformat_close_input(&input);
    if (video < 0) {
        return Status::failure(ErrorCode::Unsupported,
                               "That source answered, but has no video stream");
    }
    return Status::success();
}

Result<CaptureOutcome> CaptureSession::record(const CameraSource& camera,
                                              const std::filesystem::path& directory,
                                              const std::string& filenamePrefix,
                                              const CaptureSettings& settings,
                                              const CaptureProgressCallback& progress) {
    using ResultType = Result<CaptureOutcome>;
    stopRequested_.store(false, std::memory_order_relaxed);

    // Refused before anything is created. A capture that produced an empty file
    // and then failed would leave an operator wondering what they had recorded.
    if (!carriesVideo(camera.transport)) {
        return ResultType::failure(
            ErrorCode::Unsupported, "A Bluetooth link cannot carry video",
            "Bluetooth discovers, pairs with and commands a camera; the video comes over WiFi or "
            "a cable afterwards. Point the capture at the address the camera streams on.");
    }
    if (!camera.valid()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "No camera address to record from");
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return ResultType::failure(ErrorCode::IoError,
                                   "Unable to create the capture directory: " + directory.string(),
                                   ec.message());
    }

    CaptureOutcome outcome;
    const std::int64_t startedMs = nowMs();
    int segmentIndex = 0;
    bool everConnected = false;

    // The outer loop owns one *connection*; the inner one owns one *file*. They
    // are separate because a file boundary and a connection boundary are
    // different events: rolling to a new file at a size limit must not drop the
    // camera, and a dropped camera must not be presented as a routine roll.
    while (true) {
        if (stopRequested()) {
            outcome.stoppedByOperator = true;
            break;
        }
        if (settings.maximumDurationMs > 0 && nowMs() - startedMs >= settings.maximumDurationMs) {
            break;
        }

        // ------------------------------------------------------ connect
        const std::int64_t connectStartedMs = nowMs();
        Status opened = openInput(&impl_->input, camera, settings.openTimeoutSeconds,
                                  &stopRequested_);
        while (!opened && !stopRequested()) {
            // Only a source that has worked at least once is worth retrying: a
            // wrong address should fail now, not thirty seconds from now.
            if (!everConnected) break;
            if (nowMs() - connectStartedMs >= settings.reconnectWindowMs) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(settings.reconnectDelayMs));
            opened = openInput(&impl_->input, camera, settings.openTimeoutSeconds,
                               &stopRequested_);
        }

        if (!opened) {
            if (stopRequested()) {
                // The operator stopped while TRACE was reconnecting. That is not
                // a failed capture and it is not a gap in the recording: nothing
                // was missed, because nothing was being recorded. Reporting it
                // as a failure would put a fabricated gap into the provenance of
                // everything already filed.
                outcome.stoppedByOperator = true;
                break;
            }
            if (!everConnected) return ResultType(opened.error());
            // It had been working and has not come back. That ends the capture,
            // and it is recorded as an ending — not as a gap.
            //
            // A gap is a hole *between* two recorded segments: time an examiner
            // would otherwise read straight across. Nothing follows this one, so
            // calling it a gap would mark every recording that ends by the
            // camera being switched off as internally discontinuous, and put
            // that claim into the provenance of segments that have no missing
            // time in them at all. What actually happened is in failureReason,
            // and the retry time is the difference between wallClockMs and the
            // recorded duration.
            outcome.failureReason = "The camera did not come back: " + opened.error().message();
            break;
        }

        if (everConnected) {
            // Reconnected after a drop. The time it was away is recorded at the
            // point in the recording where it happened.
            outcome.gaps.push_back(CaptureGap{outcome.recordedDurationUs(),
                                              nowMs() - connectStartedMs, "camera reconnected"});
        }
        everConnected = true;

        const int videoStream =
            av_find_best_stream(impl_->input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStream < 0) {
            impl_->closeInput();
            return ResultType::failure(ErrorCode::Unsupported,
                                       "That camera has no video stream to record");
        }

        // Set when the connection itself has ended and the outer loop should
        // reconnect or finish. A roll to the next file leaves it false.
        bool connectionFinished = false;
        bool linkLost = false;

        // ------------------------------------------- one file per iteration
        while (!connectionFinished) {

            // ------------------------------------------------------ open output
            CaptureSegment segment;
            segment.path = directory / segmentName(filenamePrefix, segmentIndex);
            segment.startedAt = nowIso8601Utc();

            if (avformat_alloc_output_context2(&impl_->output, nullptr, "matroska",
                                               segment.path.string().c_str()) < 0 ||
                impl_->output == nullptr) {
                impl_->closeInput();
                return ResultType::failure(ErrorCode::Internal,
                                           "Unable to prepare the recording container");
            }

            impl_->streamMap.assign(static_cast<std::size_t>(impl_->input->nb_streams), -1);
            for (unsigned i = 0; i < impl_->input->nb_streams; ++i) {
                AVStream* in = impl_->input->streams[i];
                const AVMediaType type = in->codecpar->codec_type;
                if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) continue;

                AVStream* out = avformat_new_stream(impl_->output, nullptr);
                if (out == nullptr) break;
                // Parameters copied wholesale: the packets are the camera's own
                // bitstream and are written through untouched.
                if (avcodec_parameters_copy(out->codecpar, in->codecpar) < 0) break;
                out->codecpar->codec_tag = 0;
                out->time_base = in->time_base;
                impl_->streamMap[i] = out->index;
            }

            if ((impl_->output->oformat->flags & AVFMT_NOFILE) == 0) {
                if (const int rc = avio_open(&impl_->output->pb, segment.path.string().c_str(),
                                              AVIO_FLAG_WRITE);
                    rc < 0) {
                    impl_->closeOutput();
                    impl_->closeInput();
                    return ResultType::failure(ErrorCode::IoError,
                                               "Unable to write the recording: " + segment.path.string(),
                                               ffmpegErrorString(rc));
                }
            }
            if (const int rc = avformat_write_header(impl_->output, nullptr); rc < 0) {
                impl_->closeOutput();
                impl_->closeInput();
                return ResultType::failure(ErrorCode::MediaError,
                                           "Unable to start the recording container",
                                           ffmpegErrorString(rc));
            }

            // ------------------------------------------------------ copy packets
            AVPacket* packet = av_packet_alloc();
            if (packet == nullptr) {
                impl_->closeOutput();
                impl_->closeInput();
                return ResultType::failure(ErrorCode::Internal, "Unable to allocate a capture packet");
            }

            Microseconds firstPtsUs = -1;
            Microseconds lastPtsUs = 0;
            bool rollSegment = false;

            while (true) {
                if (stopRequested()) {
                    outcome.stoppedByOperator = true;
                    break;
                }
                if (settings.maximumDurationMs > 0 &&
                    nowMs() - startedMs >= settings.maximumDurationMs) {
                    break;
                }

                const int read = av_read_frame(impl_->input, packet);
                if (read < 0) {
                    if (stopRequested()) {
                        // The interrupt callback aborted the read. The operator
                        // stopped; the camera did not go away.
                        outcome.stoppedByOperator = true;
                        break;
                    }
                    // EOF from a live source means the camera stopped sending. For a
                    // local device that is the end; for a network camera it is a
                    // dropped link worth retrying.
                    linkLost = camera.transport == CameraTransport::NetworkStream;
                    break;
                }

                const int mapped = packet->stream_index >= 0 &&
                                           static_cast<std::size_t>(packet->stream_index) <
                                               impl_->streamMap.size()
                                       ? impl_->streamMap[static_cast<std::size_t>(packet->stream_index)]
                                       : -1;
                if (mapped < 0) {
                    av_packet_unref(packet);
                    continue;
                }

                AVStream* in = impl_->input->streams[packet->stream_index];
                AVStream* out = impl_->output->streams[mapped];

                if (packet->stream_index == videoStream && packet->pts != AV_NOPTS_VALUE) {
                    const Microseconds pts =
                        av_rescale_q(packet->pts, in->time_base, AVRational{1, 1'000'000});
                    if (firstPtsUs < 0) firstPtsUs = pts;
                    // Cameras hand out timestamps from their own clock, which may
                    // start anywhere; the segment is measured from its own first
                    // packet rather than from whatever the camera happened to say.
                    lastPtsUs = pts - firstPtsUs;
                    ++segment.framesWritten;
                }

                segment.bytesWritten += packet->size;
                packet->stream_index = mapped;
                av_packet_rescale_ts(packet, in->time_base, out->time_base);
                packet->pos = -1;

                const int written = av_interleaved_write_frame(impl_->output, packet);
                av_packet_unref(packet);
                if (written < 0) {
                    outcome.failureReason =
                        "Writing the recording failed: " + ffmpegErrorString(written);
                    break;
                }

                if (settings.segmentBytes > 0 && segment.bytesWritten >= settings.segmentBytes) {
                    rollSegment = true;
                    break;
                }

                if (progress) {
                    CaptureProgress state;
                    state.segmentIndex = segmentIndex;
                    state.segmentDurationUs = lastPtsUs;
                    state.bytesWritten = segment.bytesWritten;
                    state.framesWritten = segment.framesWritten;
                    state.gapsSoFar = static_cast<int>(outcome.gaps.size());
                    state.connected = true;
                    if (!progress(state)) {
                        outcome.stoppedByOperator = true;
                        stopRequested_.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
            }

            av_packet_free(&packet);

            // ------------------------------------------------------ close segment
            //
            // The output only. The input stays open when this is a size-based roll,
            // because closing and reopening the camera to start a new file would
            // lose whatever arrived in between — a real gap, caused by TRACE, that
            // nothing asked for.
            av_write_trailer(impl_->output);
            impl_->closeOutput();
            if (!rollSegment) impl_->closeInput();

            segment.endedAt = nowIso8601Utc();
            segment.durationUs = lastPtsUs;
            segment.complete = true;

            if (segment.bytesWritten > 0) {
                // Hashed now, because now is the first moment there is a file to
                // hash. This attests that the recording has not changed since TRACE
                // finished writing it — not that it matches an original, because for
                // a capture there is no original.
                if (auto digest = hashFile(segment.path); digest) {
                    segment.sha256 = digest.take();
                } else {
                    logError(kComponent, "Captured segment could not be hashed",
                             JsonValue::object()
                                 .set("path", segment.path.string())
                                 .set("detail", digest.error().toString()));
                }
                outcome.segments.push_back(std::move(segment));
                ++segmentIndex;
            } else {
                // Nothing arrived. Leaving a zero-byte file behind would look like a
                // recording that captured silence rather than one that captured
                // nothing at all.
                std::filesystem::remove(segment.path, ec);
            }

            // A roll stays on this connection. Anything else ends it — including
            // having reached the time limit, which is checked here as well as at
            // the top so a roll on the last packet does not open a file that is
            // immediately discarded.
            const bool timeIsUp = settings.maximumDurationMs > 0 &&
                                  nowMs() - startedMs >= settings.maximumDurationMs;
            if (rollSegment && !timeIsUp && outcome.failureReason.empty() &&
                !outcome.stoppedByOperator && !stopRequested()) {
                continue;
            }
            connectionFinished = true;

        }  // one file per iteration

        if (!outcome.failureReason.empty()) break;
        if (outcome.stoppedByOperator) break;
        if (!linkLost) break;
        // Fall through: the outer loop reconnects and starts a new segment,
        // and the gap it records is a real one.
    }

    outcome.wallClockMs = nowMs() - startedMs;

    logInfo(kComponent, "Capture finished",
            JsonValue::object()
                .set("camera", camera.redactedUri())
                .set("segments", static_cast<std::int64_t>(outcome.segments.size()))
                .set("gaps", static_cast<std::int64_t>(outcome.gaps.size()))
                .set("recorded_us", outcome.recordedDurationUs())
                .set("wall_clock_ms", outcome.wallClockMs)
                .set("stopped_by_operator", outcome.stoppedByOperator));

    if (outcome.segments.empty() && !outcome.failureReason.empty()) {
        return ResultType::failure(ErrorCode::MediaError, "The capture recorded nothing",
                                   outcome.failureReason);
    }
    return ResultType::success(std::move(outcome));
}

}  // namespace trace
