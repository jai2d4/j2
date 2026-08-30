#include "ai/detection/models/model_manager.h"

#include <cstdlib>
#include <utility>

#include "ai/detection/models/class_taxonomy.h"
#include "core/common/logging.h"
#include "core/security/file_hasher.h"

namespace trace {
namespace {
constexpr const char* kComponent = "models";
}

ModelManager::ModelManager(std::filesystem::path modelDirectory)
    : directory_(std::move(modelDirectory)) {}

std::filesystem::path ModelManager::defaultModelDirectory(const std::filesystem::path& dataRoot) {
    if (const char* override = std::getenv("TRACE_MODEL_DIR");
        override != nullptr && *override != '\0') {
        return std::filesystem::path(override);
    }
    return dataRoot / "models";
}

const std::vector<ModelDescriptor>& ModelManager::catalogue() {
    static const std::vector<ModelDescriptor> kCatalogue = [] {
        std::vector<ModelDescriptor> catalogue;

        ModelDescriptor yoloxTiny;
        yoloxTiny.id = "yolox-tiny";
        yoloxTiny.name = "YOLOX-Tiny";
        yoloxTiny.version = "0.1.1rc0";
        yoloxTiny.family = "yolox";
        yoloxTiny.fileName = "yolox_tiny.onnx";
        // Digest of the published release artefact, verified at download time.
        yoloxTiny.expectedSha256 =
            "427cc366d34e27ff7a03e2899b5e3671425c262ea2291f88bb942bc1cc70b0f7";
        yoloxTiny.inputWidth = 416;
        yoloxTiny.inputHeight = 416;
        // The YOLOX reference preprocessing feeds BGR at 0..255 with no
        // normalisation, pads with 114 and places the image at the top-left.
        yoloxTiny.inputIsBgr = true;
        yoloxTiny.inputScaledTo01 = false;
        yoloxTiny.padValue = 114;
        yoloxTiny.performsOwnNms = false;  // the exported graph has no NMS node
        yoloxTiny.strides = {8, 16, 32};
        yoloxTiny.classes = cocoClassLabels();
        yoloxTiny.licence = "Apache-2.0";
        yoloxTiny.source =
            "https://github.com/Megvii-BaseDetection/YOLOX releases/download/0.1.1rc0/yolox_tiny.onnx";
        yoloxTiny.notes =
            "Raw output [1, 3549, 85]: grid-decoded boxes with objectness and 80 COCO class "
            "scores. TRACE performs the decode and non-maximum suppression.";
        catalogue.push_back(std::move(yoloxTiny));

        ModelDescriptor yoloxSmall;
        yoloxSmall.id = "yolox-s";
        yoloxSmall.name = "YOLOX-S";
        yoloxSmall.version = "0.1.1rc0";
        yoloxSmall.family = "yolox";
        yoloxSmall.fileName = "yolox_s.onnx";
        yoloxSmall.inputWidth = 640;
        yoloxSmall.inputHeight = 640;
        yoloxSmall.inputIsBgr = true;
        yoloxSmall.inputScaledTo01 = false;
        yoloxSmall.padValue = 114;
        yoloxSmall.performsOwnNms = false;
        yoloxSmall.strides = {8, 16, 32};
        yoloxSmall.classes = cocoClassLabels();
        yoloxSmall.licence = "Apache-2.0";
        yoloxSmall.source =
            "https://github.com/Megvii-BaseDetection/YOLOX releases/download/0.1.1rc0/yolox_s.onnx";
        yoloxSmall.notes =
            "Larger sibling of YOLOX-Tiny, same output convention at 640x640. Optional: install "
            "it to trade speed for accuracy.";
        catalogue.push_back(std::move(yoloxSmall));

        return catalogue;
    }();
    return kCatalogue;
}

std::optional<ModelDescriptor> ModelManager::describe(const std::string& modelId) {
    for (const auto& descriptor : catalogue()) {
        if (descriptor.id == modelId) return descriptor;
    }
    return std::nullopt;
}

std::filesystem::path ModelManager::pathFor(const ModelDescriptor& descriptor) const {
    return directory_ / descriptor.fileName;
}

std::vector<ModelDescriptor> ModelManager::installed() const {
    std::vector<ModelDescriptor> present;
    std::error_code ec;
    for (const auto& descriptor : catalogue()) {
        if (std::filesystem::exists(pathFor(descriptor), ec)) present.push_back(descriptor);
    }
    return present;
}

Result<ModelValidation> ModelManager::validate(const ModelDescriptor& descriptor) const {
    using ResultType = Result<ModelValidation>;

    ModelValidation validation;
    validation.path = pathFor(descriptor);

    std::error_code ec;
    if (!std::filesystem::exists(validation.path, ec)) {
        validation.problem = "The model file was not found at " + validation.path.string() +
                             ". Install it with scripts/fetch_models.sh, or choose another "
                             "provider.";
        logWarn(kComponent, "Model file missing",
                JsonValue::object()
                    .set("model", descriptor.id)
                    .set("path", validation.path.string()));
        return ResultType::success(std::move(validation));
    }
    validation.present = true;
    validation.fileSize = static_cast<std::int64_t>(std::filesystem::file_size(validation.path, ec));

    // Hash the artefact that would actually run — this is what gets recorded
    // against every detection it produces.
    auto hashed = hashFile(validation.path);
    if (!hashed) {
        validation.problem = "The model file could not be read: " + hashed.error().message();
        return ResultType::success(std::move(validation));
    }
    validation.sha256 = hashed.take();

    if (descriptor.expectedSha256.has_value()) {
        validation.checksumMatches = validation.sha256 == *descriptor.expectedSha256;
        if (!validation.checksumMatches) {
            validation.problem =
                "The model file does not match the digest recorded for " + descriptor.name +
                ". Expected " + *descriptor.expectedSha256 + ", found " + validation.sha256 + ".";
            logError(kComponent, "Model checksum mismatch",
                     JsonValue::object()
                         .set("model", descriptor.id)
                         .set("expected", *descriptor.expectedSha256)
                         .set("actual", validation.sha256));
        }
    } else {
        // No digest on record: the file is accepted, and its digest is recorded
        // so the run is still reproducible.
        validation.checksumMatches = true;
    }

    return ResultType::success(std::move(validation));
}

DetectionProviderConfig makeProviderConfig(const ModelDescriptor& descriptor,
                                           const std::filesystem::path& modelPath,
                                           const std::string& sha256, DevicePreference device,
                                           int intraOpThreads) {
    DetectionProviderConfig config;
    config.modelPath = modelPath;
    config.modelName = descriptor.name;
    config.modelVersion = descriptor.version;
    config.modelSha256 = sha256.empty() ? std::nullopt : std::optional<std::string>(sha256);
    config.modelFamily = descriptor.family;
    config.inputWidth = descriptor.inputWidth;
    config.inputHeight = descriptor.inputHeight;
    config.inputIsBgr = descriptor.inputIsBgr;
    config.inputScaledTo01 = descriptor.inputScaledTo01;
    config.padValue = descriptor.padValue;
    config.strides = descriptor.strides;
    config.performsOwnNms = descriptor.performsOwnNms;
    config.classes = descriptor.classes;
    config.device = device;
    config.intraOpThreads = intraOpThreads;
    return config;
}

}  // namespace trace
