#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ai/detection/detection_provider.h"
#include "ai/detection/preprocessing/letterbox.h"

namespace trace {

/// Local object detection with ONNX Runtime.
///
/// Runs an interchangeable ONNX model on this workstation. The runtime is an
/// implementation detail of this class: nothing outside it knows that ONNX
/// Runtime exists, which is what allows a TensorRT or remote provider to be
/// added later without touching evidence, storage, timeline or audit code.
///
/// Device selection is honest. When a GPU is asked for and the CUDA execution
/// provider is not available in this build, the provider says so and falls back
/// to CPU, and the device that actually ran the work is what gets recorded on
/// the analysis run.
class OnnxDetectionProvider : public IDetectionProvider {
public:
    static constexpr const char* kProviderId = "onnxruntime";

    OnnxDetectionProvider();
    ~OnnxDetectionProvider() override;

    ProviderInfo info() const override;
    DetectionCapabilities capabilities() const override;
    Status initialise(const DetectionProviderConfig& config) override;
    bool isReady() const override;
    Result<DetectionBatchResult> analyze(const FrameInput& frame,
                                         const DetectionOptions& options) override;
    void shutdown() override;

    /// Execution providers compiled into the linked ONNX Runtime, e.g.
    /// "CPUExecutionProvider", "CUDAExecutionProvider".
    static std::vector<std::string> availableExecutionProviders();
    static bool cudaAvailable();
    static std::string runtimeVersion();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace trace
