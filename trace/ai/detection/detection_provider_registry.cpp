#include "ai/detection/detection_provider_registry.h"

#include <algorithm>

#include "ai/detection/providers/mock_detection_provider.h"
#include "core/common/logging.h"

#ifdef TRACE_WITH_ONNXRUNTIME
#include "ai/detection/providers/onnx_detection_provider.h"
#endif

namespace trace {

DetectionProviderRegistry& DetectionProviderRegistry::instance() {
    static DetectionProviderRegistry registry;
    return registry;
}

Status DetectionProviderRegistry::registerProvider(RegisteredDetectionProvider provider) {
    if (provider.id.empty() || provider.factory == nullptr) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "A detection provider needs an id and a factory");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing =
        std::find_if(providers_.begin(), providers_.end(),
                     [&](const RegisteredDetectionProvider& candidate) {
                         return candidate.id == provider.id;
                     });
    if (existing != providers_.end()) {
        return Status::failure(ErrorCode::AlreadyExists,
                               "A detection provider with id " + provider.id +
                                   " is already registered");
    }
    logInfo("analysis", "Detection provider registered",
            JsonValue::object().set("id", provider.id).set("name", provider.displayName));
    providers_.push_back(std::move(provider));
    return Status::success();
}

bool DetectionProviderRegistry::unregisterProvider(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::remove_if(
        providers_.begin(), providers_.end(),
        [&](const RegisteredDetectionProvider& candidate) { return candidate.id == id; });
    if (it == providers_.end()) return false;
    providers_.erase(it, providers_.end());
    return true;
}

std::vector<RegisteredDetectionProvider> DetectionProviderRegistry::providers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_;
}

DetectionProviderPtr DetectionProviderRegistry::create(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& provider : providers_) {
        if (provider.id == id) return provider.factory();
    }
    return nullptr;
}

bool DetectionProviderRegistry::contains(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(providers_.begin(), providers_.end(),
                       [&](const RegisteredDetectionProvider& p) { return p.id == id; });
}

bool DetectionProviderRegistry::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.empty();
}

void registerBuiltinDetectionProviders() {
    auto& registry = DetectionProviderRegistry::instance();

#ifdef TRACE_WITH_ONNXRUNTIME
    if (!registry.contains(OnnxDetectionProvider::kProviderId)) {
        RegisteredDetectionProvider onnx;
        onnx.id = OnnxDetectionProvider::kProviderId;
        onnx.displayName = "ONNX Runtime (local)";
        onnx.description =
            "Runs an ONNX detection model on this workstation. CPU execution today; the CUDA "
            "execution provider is selected automatically when a GPU-enabled build is present.";
        onnx.requiresModel = true;
        onnx.sendsDataOffDevice = false;
        onnx.factory = [] { return std::make_shared<OnnxDetectionProvider>(); };
        registry.registerProvider(std::move(onnx));
    }
#endif

    if (!registry.contains(MockDetectionProvider::kProviderId)) {
        RegisteredDetectionProvider mock;
        mock.id = MockDetectionProvider::kProviderId;
        mock.displayName = "Deterministic mock (testing)";
        mock.description =
            "Emits fixed detections without running a model. Present so the pipeline and the "
            "interface can be exercised without a model or a GPU; not a detector.";
        mock.requiresModel = false;
        mock.sendsDataOffDevice = false;
        mock.factory = [] { return std::make_shared<MockDetectionProvider>(); };
        registry.registerProvider(std::move(mock));
    }
}

}  // namespace trace
