#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ai/detection/detection_types.h"
#include "core/common/result.h"

namespace trace {

/// Everything TRACE needs to know about a detection model artefact.
///
/// A model is a runtime asset, not part of the application: it has a location, a
/// checksum, an input geometry, a class list and a licence. The `family` field
/// names the decoding strategy for its raw output, which is what lets a provider
/// support several models without special-casing any of them by name.
struct ModelDescriptor {
    std::string id;            ///< stable key, e.g. "yolox-tiny"
    std::string name;          ///< display name
    std::string version;
    std::string family;        ///< output decode strategy, e.g. "yolox"
    std::string fileName;      ///< file inside the model directory
    /// Digest recorded in the catalogue. When set, a mismatch blocks the run:
    /// two files with the same name are not necessarily the same artefact.
    std::optional<std::string> expectedSha256;
    int inputWidth = 0;
    int inputHeight = 0;
    bool inputIsBgr = false;   ///< channel order the model was trained on
    bool inputScaledTo01 = false;
    std::uint8_t padValue = 114;
    bool performsOwnNms = false;
    std::vector<int> strides;  ///< for grid-decoded families
    std::vector<std::string> classes;
    std::string licence;
    std::string source;
    std::string notes;
};

/// Result of checking a model file before it is used.
struct ModelValidation {
    bool present = false;
    bool checksumMatches = false;   ///< true when no expected digest was recorded
    std::string sha256;             ///< digest of the file that would actually run
    std::int64_t fileSize = 0;
    std::filesystem::path path;
    std::string problem;            ///< empty when usable
    bool usable() const { return present && checksumMatches && problem.empty(); }
};

/// Locates, describes and verifies model artefacts.
///
/// TRACE never downloads a model on its own: an absent model is reported as
/// absent, with the location it was expected in and the command that fetches it.
class ModelManager {
public:
    explicit ModelManager(std::filesystem::path modelDirectory);

    /// Default location: `<data root>/models`, overridable with the
    /// TRACE_MODEL_DIR environment variable.
    static std::filesystem::path defaultModelDirectory(const std::filesystem::path& dataRoot);

    const std::filesystem::path& directory() const { return directory_; }
    void setDirectory(std::filesystem::path directory) { directory_ = std::move(directory); }

    /// Every model TRACE knows how to drive, present or not.
    static const std::vector<ModelDescriptor>& catalogue();
    static std::optional<ModelDescriptor> describe(const std::string& modelId);

    /// Catalogue entries whose file exists in the model directory.
    std::vector<ModelDescriptor> installed() const;
    std::filesystem::path pathFor(const ModelDescriptor& descriptor) const;

    /// Verifies presence and hashes the file that would run. This digest is what
    /// gets recorded on the analysis run.
    Result<ModelValidation> validate(const ModelDescriptor& descriptor) const;

private:
    std::filesystem::path directory_;
};

/// Builds the provider configuration for a validated model. The digest passed in
/// is the one computed from the file that will actually run, so what the
/// provider reports and what the analysis run records cannot disagree.
DetectionProviderConfig makeProviderConfig(const ModelDescriptor& descriptor,
                                           const std::filesystem::path& modelPath,
                                           const std::string& sha256, DevicePreference device,
                                           int intraOpThreads = 0);

}  // namespace trace
