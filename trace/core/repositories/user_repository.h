#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/security/password.h"
#include "core/security/user_context.h"

namespace trace {

/// An account together with its credential material.
///
/// Deliberately a different type from UserAccount, which is the identity the
/// rest of TRACE carries around. Nothing outside authentication needs the hash,
/// and an identity type that cannot hold one cannot leak it into a log line, an
/// audit record or an exported report.
struct StoredAccount {
    UserAccount account;
    std::optional<password::StoredPassword> credential;
    bool mustChangePassword = false;
    int failedAttempts = 0;
    std::optional<std::string> lockedUntil;
    std::optional<std::string> lastLoginAt;
    std::optional<std::string> passwordChangedAt;

    /// An account with no credential has never had one set. It cannot be logged
    /// into — only claimed, through first-run setup.
    bool hasCredential() const { return credential.has_value(); }
};

/// Local account records.
///
/// The identity half of this class predates authentication: it registered the
/// workstation operator so audit rows could name somebody. The credential half
/// takes or returns only *derived* material — no plaintext password reaches this
/// class, which is what keeps one out of a prepared statement or a database dump.
class UserRepository {
public:
    explicit UserRepository(std::shared_ptr<Database> database);

    /// Inserts the account, or refreshes its display name / last-seen stamp.
    /// Returns the stored account (with the persisted id).
    ///
    /// Deliberately does not touch credential columns: refreshing an identity
    /// must never be able to clear a password.
    Result<UserAccount> upsert(const UserAccount& account);
    Result<std::optional<UserAccount>> findByUsername(const std::string& username);
    Result<std::vector<UserAccount>> list();

    // ------------------------------------------------------------ credentials

    /// The same lookup, with the credential attached. Used only by
    /// authentication; everything else uses findByUsername.
    Result<std::optional<StoredAccount>> findStoredByUsername(const std::string& username);
    Result<std::optional<StoredAccount>> findStoredById(const std::string& id);

    /// Accounts that can actually be logged into — active, and with a complete
    /// credential. Zero means TRACE has never had an account set up.
    Result<std::int64_t> countUsableAccounts();

    Status setCredential(const std::string& userId, const password::StoredPassword& credential,
                         const std::string& changedAt, bool mustChange);
    Status setRole(const std::string& userId, UserRole role);
    Status setActive(const std::string& userId, bool active);

    /// Records a failed attempt and, past the threshold, the moment the account
    /// stops accepting them.
    Status recordFailedAttempt(const std::string& userId, int attempts,
                               const std::optional<std::string>& lockedUntil);
    /// Clears the failure counter and stamps the login.
    Status recordSuccessfulLogin(const std::string& userId, const std::string& at);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
