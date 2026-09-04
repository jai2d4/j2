#include "ai/detection/providers/onnx_detection_provider.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
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

namespace {

/// What actually executed the model, as opposed to what was asked for.
///
/// ## Why asking is not enough
///
/// `GetAvailableProviders()` reports what the ONNX Runtime *build* was compiled
/// with. On a GPU whose architecture that build has no kernels for, it answers
/// "CUDA is available" — truthfully, and uselessly. Appending the CUDA provider
/// then succeeds as well, because appending only registers it. The session is
/// created without complaint, ONNX Runtime quietly assigns every node to the
/// CPU, and nothing in any of those three steps returns an error.
///
/// TRACE used to record `CUDA:0` on the strength of exactly that sequence. The
/// result was a run record, and a `deviceInUse` on every detection, naming an
/// accelerator that had not run a single node. For a tool whose entire claim is
/// that its records describe what happened, that is the wrong kind of wrong.
///
/// ## Why the profile
///
/// ONNX Runtime exposes no session-level or device-level provider query — there
/// is `GetAvailableProviders` and nothing else. The one thing that names the
/// provider which executed each node is the profile, so this runs a single
/// inference over a zero tensor with profiling on and reads the answer out.
///
/// Returns the provider name, or empty when it could not be determined — which
/// callers must not read as "CPU". Not knowing and knowing it was the CPU are
/// different facts, and only one of them should be written down as one.
std::string observedExecutionProvider(Ort::Session& session, const std::string& inputName,
                                      const std::string& outputName,
                                      const std::vector<std::int64_t>& shape,
                                      const Ort::MemoryInfo& memoryInfo) {
    try {
        std::int64_t elements = 1;
        std::vector<std::int64_t> concrete = shape;
        for (auto& dimension : concrete) {
            // A dynamic axis is reported as -1; one sample is enough to see
            // which provider picks up the work.
            if (dimension <= 0) dimension = 1;
            elements *= dimension;
        }
        if (elements <= 0 || elements > 64 * 1024 * 1024) return {};

        std::vector<float> input(static_cast<std::size_t>(elements), 0.0F);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(
            memoryInfo, input.data(), input.size(), concrete.data(), concrete.size());

        const char* inputNames[] = {inputName.c_str()};
        const char* outputNames[] = {outputName.c_str()};
        session.Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 1);

        Ort::AllocatorWithDefaultOptions allocator;
        // Also stops profiling, which matters: left on, it would keep writing a
        // record of every frame of a real analysis run.
        auto profilePath = session.EndProfilingAllocated(allocator);
        if (profilePath.get() == nullptr) return {};

        const std::filesystem::path profile(profilePath.get());
        std::ifstream in(profile, std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        in.close();

        std::error_code ec;
        std::filesystem::remove(profile, ec);

        if (contents.empty()) return {};
        // The profile names a provider per node, so a provider that ran nothing
        // does not appear. Checked in the order that matters: any CUDA node at
        // all means the accelerator did work.
        if (contents.find("CUDAExecutionProvider") != std::string::npos) return "CUDA:0";
        if (contents.find("CPUExecutionProvider") != std::string::npos) return "CPU";
        return {};
    } catch (const std::exception&) {
        // A probe that fails tells us nothing, which is different from telling
        // us the CPU ran it.
        return {};
    }
}

}  // namespace

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
        //
        // Nothing here decides what gets recorded. Appending the provider says
        // it was offered to the session, not that it will run anything, and the
        // difference is the whole reason observedExecutionProvider exists.
        bool cudaRequested = false;
        if (config.device == DevicePreference::Gpu || config.device == DevicePreference::Auto) {
            if (cudaAvailable()) {
                try {
                    OrtCUDAProviderOptions cudaOptions{};
                    options.AppendExecutionProvider_CUDA(cudaOptions);
                    cudaRequested = true;
                    // Only when a GPU is in play: profiling costs a file and an
                    // inference, and on the CPU path there is nothing to doubt.
                    options.EnableProfiling(ORT_TSTR("trace-ep-probe"));
                } catch (const Ort::Exception& error) {
                    logWarn(kComponent, "CUDA execution provider unavailable; using CPU",
                            JsonValue::object().set("detail", error.what()));
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

        // ------------------------------------------------- what actually ran
        //
        // Asked after the session exists, because before it exists there is
        // nothing to ask. A provider that was offered and a provider that
        // executed are different facts, and only the second is true enough to
        // put in a run record.
        if (cudaRequested) {
            const std::string observed = observedExecutionProvider(
                *impl_->session, impl_->inputName, impl_->outputName, inputShape,
                impl_->memoryInfo);
            if (observed == "CUDA:0") {
                deviceInUse = "CUDA:0";
            } else if (observed == "CPU") {
                // The documented failure on a GPU whose architecture this build
                // has no kernels for. It is not an error — the analysis runs
                // correctly, just not where the operator expected — so it is
                // reported rather than refused.
                logWarn(kComponent,
                        "CUDA was requested and accepted, but the model ran on the CPU. This "
                        "ONNX Runtime build has no kernels for this GPU's architecture.",
                        JsonValue::object().set("recorded_device", "CPU"));
                deviceInUse = "CPU";
            } else {
                // The probe could not tell. Saying CPU would be a guess and
                // saying CUDA would be the original defect, so the record says
                // what is true: it was requested, and it is unconfirmed.
                logWarn(kComponent,
                        "CUDA was requested but which provider executed could not be confirmed");
                deviceInUse = "CUDA:0 (unconfirmed)";
            }
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
        // "unconfirmed" still means an accelerator was in play, so downstream
        // records treat it as one rather than silently demoting it to CPU.
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
