#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/evidence.h"

namespace trace {

class EvidenceRepository {
public:
    explicit EvidenceRepository(std::shared_ptr<Database> database);

    Status insert(const Evidence& value);
    Status update(const Evidence& value);
    Status remove(const std::string& evidenceId);

    /// Records the outcome of an integrity check. The stored digest is never
    /// touched here — only the status columns — so a failed verification can
    /// never quietly become the new baseline.
    Status updateIntegrityStatus(const std::string& evidenceId, IntegrityStatus status,
                                 const std::string& verifiedAt);
    Status updateMediaStatus(const std::string& evidenceId, MediaStatus status);

    Result<std::optional<Evidence>> findById(const std::string& evidenceId);
    Result<std::vector<Evidence>> listForCase(const std::string& caseId);
    /// Duplicate detection: the same bytes may already be in this case.
    Result<std::vector<Evidence>> findByHash(const std::string& sha256,
                                             const std::string& caseId = {});
    Result<std::int64_t> countForCase(const std::string& caseId);

    /// Next EVD-nnnnnn label within a case.
    Result<std::string> nextEvidenceNumber(const std::string& caseId);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
