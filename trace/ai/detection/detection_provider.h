#pragma once

#include <functional>
#include <memory>

#include "ai/detection/detection_types.h"
#include "core/common/result.h"

namespace trace {

/// The extension point for object detection.
///
/// TRACE core knows nothing about any particular model, runtime or vendor. A
/// provider is handed decoded frames and returns observations in source-frame
/// coordinates; everything about how it does that — ONNX Runtime, TensorRT, a
/// remote service — lives behind this interface. Adding a runtime later means
/// adding a class, not changing the evidence, storage, timeline or audit code.
///
/// Implementations must be usable from a worker thread. The pipeline owns one
/// provider instance and drives it from a single thread; providers that are
/// internally thread-safe may declare batching support.
class IDetectionProvider {
public:
    virtual ~IDetectionProvider() = default;

    virtual ProviderInfo info() const = 0;
    virtual DetectionCapabilities capabilities() const = 0;

    /// Loads the model and acquires the device. Must fail loudly rather than
    /// silently degrading: a run that could not load its model is a failed run.
    virtual Status initialise(const DetectionProviderConfig& config) = 0;
    virtual bool isReady() const = 0;

    /// Runs inference over one frame. Returned boxes are normalised to the
    /// source frame, with the provider's own letterboxing already undone.
    virtual Result<DetectionBatchResult> analyze(const FrameInput& frame,
                                                 const DetectionOptions& options) = 0;

    virtual void shutdown() {}
};

using DetectionProviderPtr = std::shared_ptr<IDetectionProvider>;

/// Creates a fresh, uninitialised provider instance. Registered by name so the
/// UI can list what is installed without constructing anything.
using DetectionProviderFactory = std::function<DetectionProviderPtr()>;

}  // namespace trace
