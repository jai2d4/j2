#include "ai/detection/providers/onnx_detection_provider.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <mutex>

#include "ai/detection/models/class_taxonomy.h"
#include "ai/detection/postprocessing/nms.h"
#include "core/common/logging.h"

namespace trace {
namespace {

constexpr const char* kComponent = "onnx";

/// One environment per process; sessions are per provider instance.
Ort::Env& environment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "trace");
    return env;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += ", ";
        out += names[i];
    }
    return out;
}

}  // namespace

struct OnnxDetectionProvider::Impl {
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::string inputName;
    std::string outputName;
    std::vector<float> inputTensor;

    DetectionProviderConfig config;
    ProviderInfo info;
    DetectionCapabilities capabilities;
    bool ready = false;
    std::mutex mutex;
};

OnnxDetectionProvider::OnnxDetectionProvider() : impl_(std::make_unique<Impl>()) {}

OnnxDetectionProvider::~OnnxDetectionProvider() { shutdown(); }

std::vector<std::string> OnnxDetectionProvider::availableExecutionProviders() {
    try {
        return Ort::GetAvailableProviders();
    } catch (const std::exception&) {
        return {};
    }
}

bool OnnxDetectionProvider::cudaAvailable() {
    const auto providers = availableExecutionProviders();
    return std::find(providers.begin(), providers.end(), "CUDAExecutionProvider") != providers.end();
}

std::string OnnxDetectionProvider::runtimeVersion() {
    return std::string("ONNX Runtime ") + OrtGetApiBase()->GetVersionString();
}

bool OnnxDetectionProvider::isReady() const { return impl_ != nullptr && impl_->ready; }

ProviderInfo OnnxDetectionProvider::info() const {
    return impl_ != nullptr ? impl_->info : ProviderInfo{};
}

DetectionCapabilities OnnxDetectionProvider::capabilities() const {
    if (impl_ != nullptr && impl_->ready) return impl_->capabilities;

    DetectionCapabilities capabilities;
    capabilities.supportedClasses = cocoClassLabels();
    capabilities.supportsGpu = cudaAvailable();
    capabilities.supportsBatching = false;
    capabilities.sendsDataOffDevice = false;
    return capabilities;
}

Status OnnxDetectionProvider::initialise(const DetectionProviderConfig& config) {
    if (impl_ == nullptr) impl_ = std::make_unique<Impl>();
    std::lock_guard<std::mutex> lock(impl_->mutex);

    impl_->config = config;
    impl_->ready = false;

    std::error_code ec;
    if (config.modelPath.empty() || !std::filesystem::exists(config.modelPath, ec)) {
        return Status::failure(ErrorCode::NotFound,
                               "The AI model could not be loaded: no file at " +
                                   config.modelPath.string());
    }
    if (config.inputWidth <= 0 || config.inputHeight <= 0) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "The model descriptor does not state an input size");
    }

    std::string deviceInUse = "CPU";
    try {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (config.intraOpThreads > 0) options.SetIntraOpNumThreads(config.intraOpThreads);

        // GPU when asked for and genuinely available; otherwise say so and use
        // the CPU rather than failing the whole analysis.
        if (config.device == DevicePreference::Gpu || config.device == DevicePreference::Auto) {
            if (cudaAvailable()) {
                try {
                    OrtCUDAProviderOptions cudaOptions{};
                    options.AppendExecutionProvider_CUDA(cudaOptions);
                    deviceInUse = "CUDA:0";
                } catch (const Ort::Exception& error) {
                    logWarn(kComponent, "CUDA execution provider unavailable; using CPU",
                            JsonValue::object().set("detail", error.what()));
                    deviceInUse = "CPU";
                }
            } else if (config.device == DevicePreference::Gpu) {
                logWarn(kComponent,
                        "GPU inference was requested but this ONNX Runtime build has no CUDA "
                        "execution provider; using CPU");
            }
        }

        impl_->session =
            std::make_unique<Ort::Session>(environment(), config.modelPath.c_str(), options);

        Ort::AllocatorWithDefaultOptions allocator;
        if (impl_->session->GetInputCount() < 1 || impl_->session->GetOutputCount() < 1) {
            return Status::failure(ErrorCode::Unsupported,
                                   "The model does not expose an input and an output tensor");
        }
        impl_->inputName = impl_->session->GetInputNameAllocated(0, allocator).get();
        impl_->outputName = impl_->session->GetOutputNameAllocated(0, allocator).get();

        const auto inputShape =
            impl_->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const auto outputShape =
            impl_->session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();

        if (inputShape.size() != 4) {
            return Status::failure(ErrorCode::Unsupported,
                                   "The model input is not a 4-dimensional NCHW tensor");
        }
        // A fixed-shape model must agree with the descriptor, or boxes would be
        // decoded against the wrong grid.
        if (inputShape[2] > 0 && inputShape[3] > 0 &&
            (inputShape[2] != config.inputHeight || inputShape[3] != config.inputWidth)) {
            return Status::failure(
                ErrorCode::Unsupported,
                "The model expects a different input size than its descriptor states",
                "model " + std::to_string(inputShape[3]) + "x" + std::to_string(inputShape[2]) +
                    ", descriptor " + std::to_string(config.inputWidth) + "x" +
                    std::to_string(config.inputHeight));
        }
        if (outputShape.size() != 3) {
            return Status::failure(ErrorCode::Unsupported,
                                   "This model's output layout is not supported by the "
                                   "grid-decoding strategy in its descriptor");
        }

        impl_->info = ProviderInfo{};
        impl_->info.name = "ONNX Runtime (local)";
        impl_->info.version = runtimeVersion();
        impl_->info.runtime = runtimeVersion();
        impl_->info.modelName = config.modelName;
        impl_->info.modelVersion = config.modelVersion;
        impl_->info.modelSha256 = config.modelSha256;
        impl_->info.modelPath = config.modelPath.string();
        impl_->info.deviceInUse = deviceInUse;
        impl_->info.acceleratorInUse = deviceInUse != "CPU";

        impl_->capabilities = DetectionCapabilities{};
        impl_->capabilities.supportedClasses =
            config.classes.empty() ? cocoClassLabels() : config.classes;
        impl_->capabilities.supportsGpu = cudaAvailable();
        impl_->capabilities.supportsBatching = false;
        impl_->capabilities.performsOwnNms = config.performsOwnNms;
        impl_->capabilities.inputWidth = config.inputWidth;
        impl_->capabilities.inputHeight = config.inputHeight;
        impl_->capabilities.sendsDataOffDevice = false;

        impl_->ready = true;
    } catch (const Ort::Exception& error) {
        impl_->session.reset();
        return Status::failure(ErrorCode::MediaError, "The AI model could not be loaded",
                               error.what());
    } catch (const std::exception& error) {
        impl_->session.reset();
        return Status::failure(ErrorCode::Internal, "The AI model could not be loaded",
                               error.what());
    }

    logInfo(kComponent, "Detection model loaded",
            JsonValue::object()
                .set("model", config.modelName)
                .set("version", config.modelVersion)
                .set("device", deviceInUse)
                .set("input", std::to_string(config.inputWidth) + "x" +
                                  std::to_string(config.inputHeight))
                .set("providers", joinNames(availableExecutionProviders())));
    return Status::success();
}

Result<DetectionBatchResult> OnnxDetectionProvider::analyze(const FrameInput& frame,
                                                            const DetectionOptions& options) {
    using ResultType = Result<DetectionBatchResult>;
    if (impl_ == nullptr || !impl_->ready || impl_->session == nullptr) {
        return ResultType::failure(ErrorCode::Internal,
                                   "The detection provider was not initialised");
    }
    if (!frame.valid()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "The frame handed to the provider was empty");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto started = std::chrono::steady_clock::now();

    const LetterboxTransform transform = computeLetterbox(
        frame.width, frame.height, impl_->config.inputWidth, impl_->config.inputHeight,
        /*centred=*/false);

    letterboxToTensor(frame.rgb, frame.width, frame.height, transform, impl_->config.inputIsBgr,
                      impl_->config.inputScaledTo01, impl_->config.padValue,
                      impl_->inputTensor);

    DetectionBatchResult result;
    result.timestampUs = frame.timestampUs;
    result.frameNumber = frame.frameNumber;

    try {
        const std::array<std::int64_t, 4> inputShape = {
            1, 3, impl_->config.inputHeight, impl_->config.inputWidth};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            impl_->memoryInfo, impl_->inputTensor.data(), impl_->inputTensor.size(),
            inputShape.data(), inputShape.size());

        const char* inputNames[] = {impl_->inputName.c_str()};
        const char* outputNames[] = {impl_->outputName.c_str()};

        auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1,
                                           outputNames, 1);
        if (outputs.empty() || !outputs.front().IsTensor()) {
            return ResultType::failure(ErrorCode::MediaError,
                                       "The model returned no output tensor");
        }

        const auto shape = outputs.front().GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 3 || shape[0] != 1) {
            return ResultType::failure(ErrorCode::Unsupported,
                                       "The model output shape is not the expected [1, N, V]");
        }
        const int anchorCount = static_cast<int>(shape[1]);
        const int valuesPerAnchor = static_cast<int>(shape[2]);
        const float* data = outputs.front().GetTensorData<float>();

        auto detections = decodeYoloxOutput(
            data, anchorCount, valuesPerAnchor, impl_->config.inputWidth,
            impl_->config.inputHeight, impl_->config.strides, impl_->capabilities.supportedClasses,
            options.confidenceThreshold, transform);

        if (!impl_->capabilities.performsOwnNms) {
            detections = nonMaximumSuppression(std::move(detections), options.nmsIouThreshold,
                                               options.maximumDetectionsPerFrame);
        }

        if (!options.classFilter.empty()) {
            detections.erase(
                std::remove_if(detections.begin(), detections.end(),
                               [&](const RawDetection& detection) {
                                   return std::find(options.classFilter.begin(),
                                                    options.classFilter.end(),
                                                    detection.classLabel) ==
                                          options.classFilter.end();
                               }),
                detections.end());
        }
        result.detections = std::move(detections);
    } catch (const Ort::Exception& error) {
        return ResultType::failure(ErrorCode::MediaError, "Inference failed", error.what());
    } catch (const std::exception& error) {
        return ResultType::failure(ErrorCode::Internal, "Inference failed", error.what());
    }

    result.inferenceMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return ResultType::success(std::move(result));
}

void OnnxDetectionProvider::shutdown() {
    if (impl_ == nullptr) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->session.reset();
    impl_->ready = false;
}

}  // namespace trace
