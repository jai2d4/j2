#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ai/interfaces/analysis_types.h"
#include "core/common/result.h"

namespace trace {

/// What a provider can do, so the UI can offer only real capabilities.
struct AnalysisProviderCapabilities {
    std::vector<std::string> analysisTypes;
    std::vector<std::string> supportedMediaTypes;
    bool supportsRanges = false;
    bool supportsCancellation = false;
    bool requiresGpu = false;
    /// True when the provider sends evidence outside this workstation. TRACE
    /// surfaces this prominently: an agency deployment may forbid it entirely.
    bool sendsDataOffDevice = false;
};

/// The extension point for every future analysis capability — detection,
/// tracking, transcription, vision-language reasoning, enhancement — whether it
/// runs locally or as a service.
///
/// Phase 0 ships this interface and the registry, and no implementations. The
/// evidence core does not know about any specific vendor, model or runtime:
/// adding one means registering a provider, not editing the evidence code.
class IAnalysisProvider {
public:
    virtual ~IAnalysisProvider() = default;

    /// Stable identifier recorded in provenance rows.
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    virtual AnalysisProviderCapabilities capabilities() const = 0;

    /// Whether the provider can run at all right now (model files present,
    /// device available, licence valid). Providers report this instead of
    /// failing at the moment an analyst asks for work.
    virtual bool isAvailable() const = 0;

    /// Runs the analysis. Implementations must treat the evidence file as
    /// read-only and must populate the model/version fields of the result so the
    /// provenance chain stays complete.
    virtual Result<AnalysisResult> analyze(const AnalysisRequest& request) = 0;
};

using AnalysisProviderPtr = std::shared_ptr<IAnalysisProvider>;

}  // namespace trace
