#include "ai/interfaces/provider_registry.h"

#include <algorithm>

#include "core/common/logging.h"

namespace trace {

const char* toString(AnalysisStatus status) {
    switch (status) {
        case AnalysisStatus::Pending:     return "pending";
        case AnalysisStatus::Running:     return "running";
        case AnalysisStatus::Completed:   return "completed";
        case AnalysisStatus::Failed:      return "failed";
        case AnalysisStatus::Cancelled:   return "cancelled";
        case AnalysisStatus::Unsupported: return "unsupported";
    }
    return "pending";
}

AnalysisProviderRegistry& AnalysisProviderRegistry::instance() {
    static AnalysisProviderRegistry registry;
    return registry;
}

Status AnalysisProviderRegistry::registerProvider(AnalysisProviderPtr provider) {
    if (provider == nullptr) {
        return Status::failure(ErrorCode::InvalidArgument, "Provider must not be null");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = std::find_if(providers_.begin(), providers_.end(),
                                       [&](const AnalysisProviderPtr& candidate) {
                                           return candidate->name() == provider->name();
                                       });
    if (existing != providers_.end()) {
        return Status::failure(ErrorCode::AlreadyExists,
                               "An analysis provider named " + provider->name() +
                                   " is already registered");
    }
    logInfo("analysis", "Analysis provider registered",
            JsonValue::object().set("provider", provider->name()).set("version", provider->version()));
    providers_.push_back(std::move(provider));
    return Status::success();
}

bool AnalysisProviderRegistry::unregisterProvider(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::remove_if(
        providers_.begin(), providers_.end(),
        [&](const AnalysisProviderPtr& candidate) { return candidate->name() == name; });
    if (it == providers_.end()) return false;
    providers_.erase(it, providers_.end());
    return true;
}

std::vector<AnalysisProviderPtr> AnalysisProviderRegistry::providers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_;
}

AnalysisProviderPtr AnalysisProviderRegistry::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(
        providers_.begin(), providers_.end(),
        [&](const AnalysisProviderPtr& candidate) { return candidate->name() == name; });
    return it == providers_.end() ? nullptr : *it;
}

std::vector<AnalysisProviderPtr> AnalysisProviderRegistry::providersFor(
    const std::string& analysisType) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AnalysisProviderPtr> matches;
    for (const auto& provider : providers_) {
        if (!provider->isAvailable()) continue;
        const auto types = provider->capabilities().analysisTypes;
        if (std::find(types.begin(), types.end(), analysisType) != types.end()) {
            matches.push_back(provider);
        }
    }
    return matches;
}

bool AnalysisProviderRegistry::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.empty();
}

}  // namespace trace
