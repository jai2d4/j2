#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/common/json.h"
#include "core/common/time_utils.h"

namespace trace {

/// Immutable pointer to a piece of source evidence, handed to an analysis
/// provider. A provider receives the managed original's path and identity — it
/// never receives write access, and TRACE will not open the file for writing on
/// its behalf.
struct EvidenceReference {
    std::string evidenceId;
    std::string evidenceNumber;
    std::string caseId;
    std::string caseNumber;
    std::string absolutePath;   ///< managed original, read-only
    std::string sha256;         ///< digest the analysis was run against
    std::string mediaType;
    std::optional<Microseconds> durationUs;
};

/// The window of media an analysis should consider. Empty bounds mean the whole
/// item.
struct AnalysisRange {
    std::optional<Microseconds> startUs;
    std::optional<Microseconds> endUs;
};

/// A request for analysis. `parameters` is provider-specific and is persisted
/// verbatim in the provenance record, so a result can always be reproduced with
/// the same inputs.
struct AnalysisRequest {
    std::string requestId;
    EvidenceReference evidence;
    std::string analysisType;   ///< provider-declared, e.g. "transcription"
    AnalysisRange range;
    JsonValue parameters = JsonValue::object();
    std::string requestedBy;
    std::string requestedAt;
};

/// One thing an analysis asserts about the evidence.
///
/// A finding is a claim, not a conclusion: it carries the timestamp it came
/// from, the model that produced it, and a confidence the provider reports.
/// Nothing in TRACE treats a finding as fact until an analyst marks it verified.
struct AnalysisFinding {
    std::string findingId;
    std::string findingType;
    Microseconds startUs = 0;
    std::optional<Microseconds> endUs;
    std::optional<std::int64_t> frameNumber;
    std::optional<double> confidence;   ///< provider-reported, 0..1
    std::string label;
    std::string description;
    /// Geometry, embeddings, transcripts — anything structured the provider
    /// wants to persist with the finding.
    JsonValue payload = JsonValue::object();
};

enum class AnalysisStatus { Pending, Running, Completed, Failed, Cancelled, Unsupported };

const char* toString(AnalysisStatus status);

/// The outcome of one analysis run, including everything the provenance chain
/// needs: which model, which version, which parameters, over which source range.
struct AnalysisResult {
    std::string requestId;
    std::string providerName;
    std::string providerVersion;
    std::string modelName;
    std::string modelVersion;
    std::string analysisType;
    AnalysisStatus status = AnalysisStatus::Pending;
    std::string startedAt;
    std::string completedAt;
    std::string errorMessage;
    JsonValue parameters = JsonValue::object();
    JsonValue runtimeInformation = JsonValue::object();  ///< device, precision, batch size...
    std::vector<AnalysisFinding> findings;
};

}  // namespace trace
