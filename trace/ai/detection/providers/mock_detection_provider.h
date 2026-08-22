#pragma once

#include "ai/detection/detection_provider.h"

namespace trace {

/// A deterministic provider used by the automated suites.
///
/// It runs no model and touches no device, so database, pipeline and UI
/// behaviour can be tested exactly and repeatably on any machine — including one
/// with no GPU and no model files. What it emits is a fixed function of the
/// frame timestamp, documented in the implementation and asserted in the tests.
///
/// It is not a stand-in for real detection and is never offered as one: its
/// provider name says "mock" and it reports no model artefact digest, because
/// there is no artefact.
class MockDetectionProvider : public IDetectionProvider {
public:
    static constexpr const char* kProviderId = "mock";

    ProviderInfo info() const override;
    DetectionCapabilities capabilities() const override;
    Status initialise(const DetectionProviderConfig& config) override;
    bool isReady() const override { return ready_; }
    Result<DetectionBatchResult> analyze(const FrameInput& frame,
                                         const DetectionOptions& options) override;
    void shutdown() override { ready_ = false; }

    /// Fixed geometry the tests assert against.
    static NormalizedBox personBox();
    static NormalizedBox vehicleBox();
    static double personConfidence();
    static double vehicleConfidence();
    static double lowConfidenceObjectScore();

private:
    bool ready_ = false;
    DevicePreference device_ = DevicePreference::Cpu;
};

}  // namespace trace
