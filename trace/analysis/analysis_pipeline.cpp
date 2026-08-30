#include "analysis/analysis_pipeline.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "ai/detection/detection_provider_registry.h"
#include "core/common/logging.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "media/ffmpeg/video_decoder.h"

namespace trace {
namespace {

constexpr const char* kComponent = "analysis";

/// Frames handed from the decoder to the model, with a bound so memory cannot
/// grow with the length of the recording.
class BoundedFrameQueue {
public:
    explicit BoundedFrameQueue(std::size_t capacity) : capacity_(capacity) {}

    /// Blocks while the queue is full. Returns false once cancelled.
    bool push(VideoFrameData frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [&] { return queue_.size() < capacity_ || cancelled_; });
        if (cancelled_) return false;
        queue_.push_back(std::move(frame));
        notEmpty_.notify_one();
        return true;
    }

    /// Blocks until a frame arrives, the producer finishes, or cancellation.
    bool pop(VideoFrameData& frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [&] { return !queue_.empty() || finished_ || cancelled_; });
        if (cancelled_ || queue_.empty()) return false;
        frame = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();
        return true;
    }

    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        notEmpty_.notify_all();
    }

    void cancel() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

private:
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<VideoFrameData> queue_;
    bool finished_ = false;
    bool cancelled_ = false;
};

std::optional<std::int64_t> toPixels(double normalised, std::int64_t extent) {
    if (extent <= 0) return std::nullopt;
    return static_cast<std::int64_t>(normalised * static_cast<double>(extent) + 0.5);
}

}  // namespace

AnalysisPipeline::AnalysisPipeline(std::shared_ptr<AnalysisService> analysis, ModelManager models,
                                   StorageLayout layout)
    : analysis_(std::move(analysis)), models_(std::move(models)), layout_(std::move(layout)) {}

Result<AnalysisOutcome> AnalysisPipeline::execute(const DetectionAnalysisRequest& request,
                                                  const AnalysisProgressCallback& progress) {
    using ResultType = Result<AnalysisOutcome>;
    const auto started = std::chrono::steady_clock::now();

    AnalysisOutcome outcome;
    std::vector<std::string> warnings;

    // ------------------------------------------------------------- provider
    auto provider = DetectionProviderRegistry::instance().create(request.providerId);
    if (provider == nullptr) {
        return ResultType::failure(ErrorCode::NotFound,
                                   "No detection provider is installed under the id '" +
                                       request.providerId + "'");
    }

    // --------------------------------------------------------- model lookup
    std::optional<ModelDescriptor> descriptor;
    ModelValidation validation;
    if (!request.modelId.empty()) {
        descriptor = ModelManager::describe(request.modelId);
        if (!descriptor) {
            return ResultType::failure(ErrorCode::NotFound,
                                       "TRACE does not have a descriptor for model '" +
                                           request.modelId + "'");
        }
        auto validated = models_.validate(*descriptor);
        if (!validated) return ResultType(validated.error());
        validation = validated.take();
    }

    // The run is created before anything can fail, so a refused model still
    // leaves a persistent, auditable record of the attempt.
    AnalysisRunDraft draft;
    draft.caseId = request.caseId;
    draft.caseNumber = request.caseNumber;
    draft.evidenceId = request.evidenceId;
    draft.evidenceNumber = request.evidenceNumber;
    draft.providerName = provider->info().name.empty() ? request.providerId : provider->info().name;
    draft.providerVersion = provider->info().version;
    draft.runtime = provider->info().runtime;
    draft.modelName = descriptor ? descriptor->name : provider->info().modelName;
    draft.modelVersion = descriptor ? descriptor->version : provider->info().modelVersion;
    draft.modelSha256 = validation.sha256.empty() ? provider->info().modelSha256
                                                  : std::optional<std::string>(validation.sha256);
    draft.modelRelPath = descriptor ? std::optional<std::string>(descriptor->fileName) : std::nullopt;
    draft.deviceRequested = toString(request.device);
    draft.evidenceSha256 = request.evidenceSha256;

    const Microseconds interval = samplingIntervalUs(request.quality, request.customIntervalUs);
    draft.samplingIntervalUs = interval;
    draft.configurationJson = JsonValue::object()
                                  .set("provider_id", request.providerId)
                                  .set("model_id", request.modelId)
                                  .set("quality", toString(request.quality))
                                  .set("sampling_interval_us", interval)
                                  .set("confidence_threshold", request.confidenceThreshold)
                                  .set("nms_iou_threshold", request.nmsIouThreshold)
                                  .set("max_detections_per_frame",
                                       static_cast<std::int64_t>(request.maximumDetectionsPerFrame))
                                  .set("device_requested", toString(request.device))
                                  .dump();

    auto startedRun = analysis_->startRun(draft);
    if (!startedRun) return ResultType(startedRun.error());
    outcome.run = startedRun.take();

    /// Records a terminal state once, and returns the outcome to the caller.
    const auto finish = [&](AnalysisRunStatus status,
                            const std::optional<std::string>& error) -> Result<AnalysisOutcome> {
        outcome.status = status;
        outcome.elapsedSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        outcome.effectiveFramesPerSecond =
            outcome.elapsedSeconds > 0.0
                ? static_cast<double>(outcome.framesAnalyzed) / outcome.elapsedSeconds
                : 0.0;
        outcome.error = error;
        outcome.warnings = warnings;
        outcome.run.status = status;

        if (auto recorded = analysis_->finishRun(outcome.run, status, request.caseNumber,
                                                 request.evidenceNumber, outcome.framesAnalyzed,
                                                 outcome.detectionsStored, error, warnings,
                                                 outcome.elapsedSeconds);
            !recorded) {
            return Result<AnalysisOutcome>(recorded.error());
        }
        if (progress) {
            AnalysisProgress update;
            update.status = status;
            update.framesAnalyzed = outcome.framesAnalyzed;
            update.detectionsFound = outcome.detectionsStored;
            update.elapsedSeconds = outcome.elapsedSeconds;
            update.framesPerSecond = outcome.effectiveFramesPerSecond;
            update.device = outcome.deviceUsed;
            update.model = outcome.run.modelName;
            update.fractionComplete = status == AnalysisRunStatus::Completed ? 1.0 : 0.0;
            update.message = error.value_or(std::string(toDisplayString(status)));
            progress(update);
        }
        return Result<AnalysisOutcome>::success(std::move(outcome));
    };

    // ---------------------------------------------------- model validation
    if (descriptor && !validation.usable()) {
        analysis_->recordModelValidationFailed(request.caseId, request.caseNumber,
                                               request.evidenceId, request.evidenceNumber,
                                               descriptor->name, validation.problem);
        return finish(AnalysisRunStatus::Failed, validation.problem);
    }

    // ------------------------------------------------------ provider set-up
    DetectionProviderConfig providerConfig;
    if (descriptor) {
        providerConfig = makeProviderConfig(*descriptor, validation.path, validation.sha256,
                                           request.device);
    } else {
        providerConfig.device = request.device;
    }
    if (auto initialised = provider->initialise(providerConfig); !initialised) {
        return finish(AnalysisRunStatus::Failed, initialised.error().toString());
    }

    outcome.deviceUsed = provider->info().deviceInUse;
    analysis_->recordDevice(outcome.run.id, outcome.deviceUsed, provider->info().runtime);
    outcome.run.deviceUsed = outcome.deviceUsed;
    outcome.run.runtime = provider->info().runtime;
    analysis_->recordModelLoaded(request.caseId, request.caseNumber, request.evidenceId,
                                 request.evidenceNumber, outcome.run.modelName,
                                 outcome.run.modelVersion, outcome.run.modelSha256,
                                 outcome.deviceUsed, provider->info().runtime);

    if (request.device == DevicePreference::Gpu && !provider->info().acceleratorInUse) {
        warnings.emplace_back(
            "GPU inference was requested but no GPU execution provider was available; the "
            "analysis ran on the CPU.");
    }

    // --------------------------------------------------------- open the media
    auto opened = VideoDecoder::open(request.mediaPath, request.mediaKey);
    if (!opened) {
        provider->shutdown();
        return finish(AnalysisRunStatus::Failed, opened.error().toString());
    }
    auto decoder = opened.take();
    const DecoderStreamInfo streamInfo = decoder->info();

    outcome.run.sourceWidth = streamInfo.width;
    outcome.run.sourceHeight = streamInfo.height;

    const Microseconds analysisLimitUs =
        request.analyseUpToUs > 0 ? request.analyseUpToUs : streamInfo.durationUs;

    DetectionOptions options;
    options.confidenceThreshold = request.confidenceThreshold;
    options.nmsIouThreshold = request.nmsIouThreshold;
    options.maximumDetectionsPerFrame = request.maximumDetectionsPerFrame;
    options.classFilter = request.classFilter;

    // ------------------------------------------------- decode / infer / store
    BoundedFrameQueue queue(kFrameQueueCapacity);
    std::atomic<bool> cancelled{false};
    const auto externalCancel = request.cancellationToken;
    const auto cancellationRequested = [&] {
        return cancelled.load() || (externalCancel != nullptr && externalCancel->load());
    };
    std::optional<std::string> decodeError;

    std::thread producer([&] {
        FrameSampler sampler(interval);
        for (;;) {
            if (cancellationRequested()) break;
            auto frame = decoder->nextFrame();
            if (!frame) {
                // NotFound is the ordinary end of the stream; anything else is a
                // decode failure that must be reported rather than hidden.
                if (frame.error().code() != ErrorCode::NotFound) {
                    decodeError = frame.error().toString();
                }
                break;
            }
            VideoFrameData data = frame.take();
            if (analysisLimitUs > 0 && data.presentationUs > analysisLimitUs) break;
            if (!sampler.accept(data.presentationUs)) continue;
            if (!queue.push(std::move(data))) break;
        }
        queue.finish();
    });

    std::vector<Detection> pending;
    pending.reserve(kDetectionBatchSize);
    std::optional<std::string> inferenceError;

    const auto flush = [&]() -> Status {
        if (pending.empty()) return Status::success();
        auto stored = analysis_->storeDetections(pending);
        if (!stored) return Status(stored.error());
        outcome.detectionsStored += stored.value();
        pending.clear();
        return Status::success();
    };

    VideoFrameData frame;
    while (queue.pop(frame)) {
        FrameInput input;
        input.rgb = frame.rgb.data();
        input.width = frame.width;
        input.height = frame.height;
        input.timestampUs = frame.presentationUs;
        input.frameNumber = frame.frameNumber;

        auto analysed = provider->analyze(input, options);
        if (!analysed) {
            inferenceError = analysed.error().toString();
            break;
        }
        ++outcome.framesAnalyzed;

        const std::string now = nowIso8601Utc();
        for (const auto& raw : analysed.value().detections) {
            Detection detection;
            detection.id = generateUuid();
            detection.analysisRunId = outcome.run.id;
            detection.caseId = request.caseId;
            detection.evidenceId = request.evidenceId;
            detection.timestampUs = analysed.value().timestampUs;
            detection.frameNumber = analysed.value().frameNumber;
            detection.classId = raw.classId;
            detection.classLabel = raw.classLabel;
            detection.classGroup = raw.classGroup;
            detection.confidence = raw.confidence;
            detection.box = raw.box;
            detection.pixelX = toPixels(raw.box.x, streamInfo.width);
            detection.pixelY = toPixels(raw.box.y, streamInfo.height);
            detection.pixelWidth = toPixels(raw.box.width, streamInfo.width);
            detection.pixelHeight = toPixels(raw.box.height, streamInfo.height);
            detection.verification = DetectionVerification::Unreviewed;
            detection.createdAt = now;
            pending.push_back(std::move(detection));
        }

        if (pending.size() >= kDetectionBatchSize) {
            if (auto status = flush(); !status) {
                inferenceError = status.error().toString();
                break;
            }
        }

        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (progress) {
            AnalysisProgress update;
            update.status = AnalysisRunStatus::Running;
            update.framesAnalyzed = outcome.framesAnalyzed;
            update.detectionsFound =
                outcome.detectionsStored + static_cast<std::int64_t>(pending.size());
            update.positionUs = frame.presentationUs;
            update.durationUs = analysisLimitUs;
            update.fractionComplete =
                analysisLimitUs > 0
                    ? std::min(1.0, static_cast<double>(frame.presentationUs) /
                                        static_cast<double>(analysisLimitUs))
                    : 0.0;
            update.elapsedSeconds = elapsed;
            update.framesPerSecond =
                elapsed > 0.0 ? static_cast<double>(outcome.framesAnalyzed) / elapsed : 0.0;
            update.device = outcome.deviceUsed;
            update.model = outcome.run.modelName;
            if (!progress(update)) cancelled.store(true);
        }
        if (cancellationRequested()) {
            cancelled.store(true);
            break;
        }
    }

    if (cancellationRequested()) cancelled.store(true);
    queue.cancel();
    if (producer.joinable()) producer.join();
    provider->shutdown();

    // Whatever was decoded before the stop is real analysis and is kept.
    if (auto status = flush(); !status && !inferenceError) {
        inferenceError = status.error().toString();
    }
    analysis_->recordProgress(outcome.run.id, outcome.framesAnalyzed, outcome.detectionsStored,
                              outcome.framesAnalyzed > 0 ? std::optional<std::int64_t>(
                                                               std::max<Microseconds>(0, frame.presentationUs))
                                                         : std::nullopt);

    if (cancelled.load()) {
        logInfo(kComponent, "Analysis cancelled by operator",
                JsonValue::object()
                    .set("run", outcome.run.id)
                    .set("frames", outcome.framesAnalyzed)
                    .set("detections", outcome.detectionsStored));
        return finish(AnalysisRunStatus::Cancelled,
                      "Cancelled by the operator after " + std::to_string(outcome.framesAnalyzed) +
                          " analysed frame(s)");
    }
    if (inferenceError) {
        return finish(outcome.framesAnalyzed > 0 ? AnalysisRunStatus::Partial
                                                 : AnalysisRunStatus::Failed,
                      *inferenceError);
    }
    if (decodeError) {
        warnings.push_back("Decoding stopped early: " + *decodeError);
        return finish(outcome.framesAnalyzed > 0 ? AnalysisRunStatus::Partial
                                                 : AnalysisRunStatus::Failed,
                      *decodeError);
    }
    if (outcome.framesAnalyzed == 0) {
        return finish(AnalysisRunStatus::Failed,
                      "No frames could be decoded from this evidence item");
    }
    return finish(AnalysisRunStatus::Completed, std::nullopt);
}

}  // namespace trace
