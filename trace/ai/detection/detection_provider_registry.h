#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ai/detection/detection_provider.h"

namespace trace {

/// A provider that is installed and can be offered to the operator.
struct RegisteredDetectionProvider {
    std::string id;            ///< stable identifier, e.g. "onnxruntime"
    std::string displayName;   ///< e.g. "ONNX Runtime (local)"
    std::string description;
    /// True when the provider needs a model artefact from the catalogue, so the
    /// UI offers a model list only where one is actually used.
    bool requiresModel = true;
    /// True when frames leave this workstation. Stated here rather than
    /// discovered at run time: an agency deployment may forbid it outright, and
    /// the operator has to be able to see it before starting a run.
    bool sendsDataOffDevice = false;
    DetectionProviderFactory factory;
};

/// Where detection providers announce themselves.
///
/// The registry holds factories rather than instances so that listing what is
/// available costs nothing — no model is loaded and no device is acquired until
/// an analysis actually starts.
class DetectionProviderRegistry {
public:
    static DetectionProviderRegistry& instance();

    Status registerProvider(RegisteredDetectionProvider provider);
    bool unregisterProvider(const std::string& id);

    std::vector<RegisteredDetectionProvider> providers() const;
    /// Creates an uninitialised instance, or nullptr when the id is unknown.
    DetectionProviderPtr create(const std::string& id) const;
    bool contains(const std::string& id) const;
    bool empty() const;

private:
    DetectionProviderRegistry() = default;

    mutable std::mutex mutex_;
    std::vector<RegisteredDetectionProvider> providers_;
};

/// Registers the providers compiled into this build: the deterministic mock
/// always, and the ONNX Runtime provider when it was compiled in.
void registerBuiltinDetectionProviders();

}  // namespace trace
