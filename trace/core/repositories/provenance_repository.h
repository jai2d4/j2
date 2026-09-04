#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/derived_asset.h"

namespace trace {

/// Storage for the provenance chain: processing operations and the derived
/// assets they produced.
class ProvenanceRepository {
public:
    explicit ProvenanceRepository(std::shared_ptr<Database> database);

    Status insertOperation(const ProcessingOperation& operation);
    Status completeOperation(const std::string& operationId, const std::string& completedAt,
                             const std::string& status,
                             const std::optional<std::string>& errorMessage = std::nullopt);
    Result<std::optional<ProcessingOperation>> findOperation(const std::string& operationId);
    Result<std::vector<ProcessingOperation>> listOperationsForEvidence(const std::string& evidenceId);

    Status insertAsset(const DerivedAsset& asset);
    Status removeAsset(const std::string& assetId);
    Result<std::optional<DerivedAsset>> findAsset(const std::string& assetId);
    Result<std::vector<DerivedAsset>> listAssetsForEvidence(const std::string& evidenceId);
    Result<std::vector<DerivedAsset>> listAssetsForCase(const std::string& caseId);
    Result<std::int64_t> countAssetsForEvidence(const std::string& evidenceId);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
