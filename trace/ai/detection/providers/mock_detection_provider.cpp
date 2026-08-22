#include "ai/detection/providers/mock_detection_provider.h"

#include <algorithm>

#include "ai/detection/models/class_taxonomy.h"
#include "trace/trace_version.h"

namespace trace {
namespace {

bool classAllowed(const DetectionOptions& options, const std::string& label) {
    if (options.classFilter.empty()) return true;
    return std::find(options.classFilter.begin(), options.classFilter.end(), label) !=
           options.classFilter.end();
}

RawDetection makeDetection(const std::string& label, double confidence, const NormalizedBox& box) {
    RawDetection detection;
    const auto& classes = cocoClassLabels();
    const auto it = std::find(classes.begin(), classes.end(), label);
    detection.classId = it == classes.end() ? -1 : static_cast<int>(std::distance(classes.begin(), it));
    detection.classLabel = label;
    detection.classGroup = classGroupForLabel(label);
    detection.confidence = confidence;
    detection.box = box;
    return detection;
}

}  // namespace

NormalizedBox MockDetectionProvider::personBox() { return NormalizedBox{0.10, 0.20, 0.20, 0.50}; }
NormalizedBox MockDetectionProvider::vehicleBox() { return NormalizedBox{0.55, 0.45, 0.35, 0.30}; }
double MockDetectionProvider::personConfidence() { return 0.90; }
double MockDetectionProvider::vehicleConfidence() { return 0.75; }
double MockDetectionProvider::lowConfidenceObjectScore() { return 0.30; }

ProviderInfo MockDetectionProvider::info() const {
    ProviderInfo info;
    info.name = "Deterministic mock detector";
    info.version = "1.0";
    info.runtime = std::string("TRACE ") + kApplicationVersion + " (built-in)";
    info.modelName = "mock-detector";
    info.modelVersion = "1.0";
    // No model artefact exists, so no artefact digest is claimed.
    info.modelSha256 = std::nullopt;
    info.modelPath = std::nullopt;
    info.deviceInUse = "CPU (no inference performed)";
    return info;
}

DetectionCapabilities MockDetectionProvider::capabilities() const {
    DetectionCapabilities capabilities;
    capabilities.supportedClasses = {"person", "car", "dog"};
    capabilities.supportsGpu = false;
    capabilities.supportsBatching = false;
    capabilities.performsOwnNms = true;  // its output never overlaps
    capabilities.inputWidth = 0;
    capabilities.inputHeight = 0;
    capabilities.sendsDataOffDevice = false;
    return capabilities;
}

Status MockDetectionProvider::initialise(const DetectionProviderConfig& config) {
    device_ = config.device;
    ready_ = true;
    return Status::success();
}

Result<DetectionBatchResult> MockDetectionProvider::analyze(const FrameInput& frame,
                                                            const DetectionOptions& options) {
    using ResultType = Result<DetectionBatchResult>;
    if (!ready_) {
        return ResultType::failure(ErrorCode::Internal, "The mock provider was not initialised");
    }
    if (!frame.valid()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "The frame handed to the provider was empty");
    }

    DetectionBatchResult result;
    result.timestampUs = frame.timestampUs;
    result.frameNumber = frame.frameNumber;

    // Deterministic by construction:
    //   * every analysed frame contains one person;
    //   * frames at an even whole second also contain one car;
    //   * every frame contains one deliberately low-confidence dog, so
    //     threshold filtering has something to remove.
    const std::int64_t wholeSeconds = frame.timestampUs / 1'000'000;

    if (personConfidence() >= options.confidenceThreshold && classAllowed(options, "person")) {
        result.detections.push_back(makeDetection("person", personConfidence(), personBox()));
    }
    if (wholeSeconds % 2 == 0 && vehicleConfidence() >= options.confidenceThreshold &&
        classAllowed(options, "car")) {
        result.detections.push_back(makeDetection("car", vehicleConfidence(), vehicleBox()));
    }
    if (lowConfidenceObjectScore() >= options.confidenceThreshold && classAllowed(options, "dog")) {
        result.detections.push_back(
            makeDetection("dog", lowConfidenceObjectScore(), NormalizedBox{0.05, 0.75, 0.10, 0.15}));
    }

    if (options.maximumDetectionsPerFrame > 0 &&
        static_cast<int>(result.detections.size()) > options.maximumDetectionsPerFrame) {
        result.detections.resize(static_cast<std::size_t>(options.maximumDetectionsPerFrame));
    }
    result.inferenceMilliseconds = 0.0;
    return ResultType::success(std::move(result));
}

}  // namespace trace
