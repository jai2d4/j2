#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/models/detection.h"

namespace trace {

/// Which device the operator asked for. What actually ran is reported back in
/// ProviderInfo::deviceInUse — TRACE records the device that did the work, not
/// the one that was requested.
enum class DevicePreference { Auto, Cpu, Gpu };

const char* toString(DevicePreference preference);
const char* toDisplayString(DevicePreference preference);
DevicePreference devicePreferenceFromString(const std::string& text,
                                            DevicePreference fallback = DevicePreference::Auto);

/// One decoded frame presented to a provider.
///
/// The pixel buffer is borrowed for the duration of the call: providers must not
/// retain the pointer. `timestampUs` is the frame's real presentation timestamp,
/// which is what every resulting detection is anchored to.
struct FrameInput {
    const std::uint8_t* rgb = nullptr;  ///< packed RGB24, row stride = width * 3
    int width = 0;
    int height = 0;
    std::int64_t timestampUs = 0;
    std::optional<std::int64_t> frameNumber;

    bool valid() const { return rgb != nullptr && width > 0 && height > 0; }
};

/// Per-request detection settings.
struct DetectionOptions {
    /// Detections below this score are discarded. This is the model's own
    /// confidence in the visual class — nothing more.
    double confidenceThreshold = 0.50;
    double nmsIouThreshold = 0.45;
    int maximumDetectionsPerFrame = 300;
    /// Empty means every class the model supports.
    std::vector<std::string> classFilter;
};

/// One object reported by a provider, before it is persisted.
struct RawDetection {
    int classId = 0;
    std::string classLabel;
    DetectionClassGroup classGroup = DetectionClassGroup::Object;
    double confidence = 0.0;
    /// Normalised to the *source* frame, not the model input canvas: providers
    /// undo their own letterboxing before returning.
    NormalizedBox box;
};

/// What a provider found in one frame.
struct DetectionBatchResult {
    std::int64_t timestampUs = 0;
    std::optional<std::int64_t> frameNumber;
    std::vector<RawDetection> detections;
    double inferenceMilliseconds = 0.0;
};

/// Identity of the thing that produced results — recorded on every analysis run
/// so a detection can always name what made it.
struct ProviderInfo {
    std::string name;
    std::string version;
    std::string runtime;        ///< e.g. "ONNX Runtime 1.17.3"
    std::string modelName;
    std::string modelVersion;
    std::optional<std::string> modelSha256;
    std::optional<std::string> modelPath;
    std::string deviceInUse;    ///< what actually ran: "CPU", "CUDA:0", ...
    /// True when a hardware accelerator did the work rather than the CPU. The
    /// provider reports this because only it knows what its device string means
    /// — nothing above this interface should be matching vendor names.
    bool acceleratorInUse = false;
};

/// What a provider can do, so the UI offers only real options.
struct DetectionCapabilities {
    std::vector<std::string> supportedClasses;
    bool supportsGpu = false;
    bool supportsBatching = false;
    /// True when the model graph already applies non-maximum suppression, in
    /// which case TRACE must not apply it a second time.
    bool performsOwnNms = false;
    int inputWidth = 0;
    int inputHeight = 0;
    /// True when the provider sends frames off this workstation. Surfaced
    /// prominently: an agency deployment may forbid it outright.
    bool sendsDataOffDevice = false;
};

/// Everything needed to bring a provider up.
struct DetectionProviderConfig {
    std::filesystem::path modelPath;
    std::string modelName;
    std::string modelVersion;
    std::optional<std::string> modelSha256;
    /// Decode strategy for the model's raw output tensor (e.g. "yolox").
    std::string modelFamily;
    int inputWidth = 0;
    int inputHeight = 0;
    /// Preprocessing the model was trained with. These are properties of the
    /// artefact, so they travel with it rather than being assumed by the runtime.
    bool inputIsBgr = false;
    bool inputScaledTo01 = false;
    std::uint8_t padValue = 114;
    /// Grid strides for grid-decoded families.
    std::vector<int> strides;
    /// True when the exported graph already applies non-maximum suppression.
    bool performsOwnNms = false;
    std::vector<std::string> classes;
    DevicePreference device = DevicePreference::Auto;
    int intraOpThreads = 0;  ///< 0 = let the runtime decide
};

}  // namespace trace
