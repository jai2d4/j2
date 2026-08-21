#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/security/user_context.h"

namespace trace {

/// Local account records. Phase 0 registers the workstation operator so audit
/// rows can be joined to an account; no credential is ever written.
class UserRepository {
public:
    explicit UserRepository(std::shared_ptr<Database> database);

    /// Inserts the account, or refreshes its display name / last-seen stamp.
    /// Returns the stored account (with the persisted id).
    Result<UserAccount> upsert(const UserAccount& account);
    Result<std::optional<UserAccount>> findByUsername(const std::string& username);
    Result<std::vector<UserAccount>> list();

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
