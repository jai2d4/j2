#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ai/interfaces/analysis_provider.h"

namespace trace {

/// Where analysis providers announce themselves.
///
/// The registry is empty in Phase 0. It exists now so that a later module can
/// add a capability without any change to the evidence, storage or audit code:
/// register a provider, and the viewer offers whatever it declares.
class AnalysisProviderRegistry {
public:
    static AnalysisProviderRegistry& instance();

    Status registerProvider(AnalysisProviderPtr provider);
    bool unregisterProvider(const std::string& name);

    std::vector<AnalysisProviderPtr> providers() const;
    AnalysisProviderPtr find(const std::string& name) const;
    /// Providers that declare `analysisType` and report themselves available.
    std::vector<AnalysisProviderPtr> providersFor(const std::string& analysisType) const;
    bool empty() const;

private:
    AnalysisProviderRegistry() = default;

    mutable std::mutex mutex_;
    std::vector<AnalysisProviderPtr> providers_;
};

}  // namespace trace
