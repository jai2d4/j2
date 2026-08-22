#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ai/detection/detection_types.h"
#include "ai/detection/models/model_manager.h"
#include "analysis/frame_sampler.h"
#include "core/common/result.h"
#include "core/models/analysis_run.h"
#include "core/services/analysis_service.h"
#include "core/storage/storage_layout.h"

namespace trace {

/// What the operator asked for.
struct DetectionAnalysisRequest {
    std::string caseId;
    std::string caseNumber;
    std::string evidenceId;
    std::string evidenceNumber;
    std::filesystem::path mediaPath;
    std::string evidenceSha256;

    std::string providerId;   ///< registry id, e.g. "onnxruntime"
    std::string modelId;      ///< catalogue id; empty when the provider needs no artefact
    DevicePreference device = DevicePreference::Auto;
    AnalysisQuality quality = AnalysisQuality::Standard;
    Microseconds customIntervalUs = 0;

    double confidenceThreshold = 0.50;
    double nmsIouThreshold = 0.45;
    int maximumDetectionsPerFrame = 300;
    std::vector<std::string> classFilter;

    /// Stop after this much media. Used by the tests to keep runs short; zero
    /// analyses the whole item.
    Microseconds analyseUpToUs = 0;

    /// Optional out-of-band stop signal. Setting it cancels the run at the next
    /// frame boundary, independently of how often progress is reported — a UI
    /// must be able to stop an analysis without waiting for the next update.
    std::shared_ptr<std::atomic<bool>> cancellationToken;
};

/// Live state of a running analysis.
struct AnalysisProgress {
    AnalysisRunStatus status = AnalysisRunStatus::Running;
    std::int64_t framesAnalyzed = 0;
    std::int64_t detectionsFound = 0;
    Microseconds positionUs = 0;
    Microseconds durationUs = 0;
    double fractionComplete = 0.0;
    double elapsedSeconds = 0.0;
    double framesPerSecond = 0.0;
    std::string device;
    std::string model;
    std::string message;
};

/// Called once per analysed frame. Return false to cancel; the request's
/// cancellation token does the same thing out of band. Cancellation is safe at
/// any point: results already written are kept and the run is recorded as
/// CANCELLED, never COMPLETED.
///
/// Implementations must be cheap and must not block: this runs on the analysis
/// thread, between frames. A UI should store the values and throttle its own
/// repaints rather than repainting from here.
using AnalysisProgressCallback = std::function<bool(const AnalysisProgress&)>;

/// What actually happened.
struct AnalysisOutcome {
    AnalysisRun run;
    AnalysisRunStatus status = AnalysisRunStatus::Failed;
    std::int64_t framesAnalyzed = 0;
    std::int64_t detectionsStored = 0;
    double elapsedSeconds = 0.0;
    double effectiveFramesPerSecond = 0.0;
    std::string deviceUsed;
    std::optional<std::string> error;
    std::vector<std::string> warnings;
};

/// Runs object detection over one evidence item.
///
///   decode ─► bounded frame queue ─► provider ─► batch ─► database
///
/// The decoder runs on its own thread and blocks when the queue is full, so a
/// 4K recording never accumulates frames in memory: the queue holds a handful,
/// and inference sets the pace. Detections are written in batches inside one
/// transaction each, because a long recording produces far too many rows for a
/// transaction apiece.
///
/// The pipeline is synchronous from the caller's point of view and does not
/// touch the UI: callers run it on a worker thread and receive progress through
/// the callback.
class AnalysisPipeline {
public:
    AnalysisPipeline(std::shared_ptr<AnalysisService> analysis, ModelManager models,
                     StorageLayout layout);

    Result<AnalysisOutcome> execute(const DetectionAnalysisRequest& request,
                                    const AnalysisProgressCallback& progress = {});

    /// Number of detections buffered before a batch is written.
    static constexpr std::size_t kDetectionBatchSize = 500;
    /// Frames held between the decoder and the model.
    static constexpr std::size_t kFrameQueueCapacity = 4;

private:
    std::shared_ptr<AnalysisService> analysis_;
    ModelManager models_;
    StorageLayout layout_;
};

}  // namespace trace
