#pragma once

#include <memory>
#include <string>

#include "core/common/result.h"
#include "core/models/evidence.h"
#include "core/repositories/evidence_repository.h"
#include "core/security/file_hasher.h"
#include "core/security/workspace_keys.h"
#include "core/services/audit_service.h"
#include "core/storage/storage_layout.h"

namespace trace {

struct IntegrityCheck {
    bool verified = false;
    std::string storedSha256;
    std::string computedSha256;
    std::string checkedAt;
    std::int64_t bytesRead = 0;
    bool fileMissing = false;
};

/// Re-verification of managed originals.
///
/// The stored digest is treated as the baseline and is never rewritten by a
/// verification, whatever the outcome. A mismatch is reported, persisted as a
/// failed status and recorded in the audit trail; deciding what it means is the
/// analyst's job, not the software's.
///
/// For encrypted evidence the comparison is against the digest of the
/// *decrypted* file, because the stored digest is the one an outside party
/// computes from the original recording. Hashing the container instead would
/// compare two unrelated numbers and fail every check.
class IntegrityService {
public:
    IntegrityService(std::shared_ptr<Database> database, StorageLayout layout,
                     std::shared_ptr<AuditService> audit,
                     std::shared_ptr<WorkspaceKeys> keys = nullptr);

    Result<IntegrityCheck> verify(const Evidence& evidence, const std::string& caseNumber,
                                  const ProgressCallback& progress = {});

private:
    std::shared_ptr<EvidenceRepository> evidence_;
    StorageLayout layout_;
    std::shared_ptr<AuditService> audit_;
    std::shared_ptr<WorkspaceKeys> keys_;
};

}  // namespace trace
