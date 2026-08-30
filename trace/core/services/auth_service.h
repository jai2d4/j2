#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/repositories/user_repository.h"
#include "core/services/audit_service.h"

namespace trace {

/// Sign-in, account creation and password changes for local accounts.
///
/// Before this existed, TRACE trusted whoever opened it: the audit trail named
/// whatever user the operating system reported, and nothing checked that the
/// person at the keyboard was that user. For software whose purpose is
/// establishing who did what to a piece of evidence, that was a hole in the
/// chain of custody rather than a missing convenience.
///
/// Every method here is auditable and none of them logs a password. The
/// plaintext exists as an argument and nowhere else — not in a log line, not in
/// an audit detail, not in an error message.
class AuthService {
public:
    /// Failed attempts before an account stops accepting them. Low enough to
    /// make online guessing hopeless, high enough that an operator mistyping
    /// their own password is not locked out of a case at an awkward moment.
    static constexpr int kMaxFailedAttempts = 5;
    /// How long a lockout lasts. A fixed window rather than an administrator
    /// unlock, so a single-operator workstation cannot lock itself out
    /// permanently with nobody able to help.
    static constexpr int kLockoutMinutes = 15;

    AuthService(std::shared_ptr<Database> database, std::shared_ptr<AuditService> audit);

    /// True when no account can be logged into yet, so the application must run
    /// first-run setup instead of showing a sign-in prompt.
    Result<bool> needsFirstRunSetup();

    /// Creates the first administrator. Refuses once any usable account exists,
    /// so the first-run path cannot be used later to mint a second
    /// administrator without authenticating.
    Result<UserAccount> createFirstAdministrator(const std::string& username,
                                                 const std::string& displayName,
                                                 const std::string& password);

    /// Verifies a credential and, on success, makes the account current.
    ///
    /// Failure is deliberately uniform: a wrong username and a wrong password
    /// return the same message and take about the same time, so the prompt
    /// cannot be used to enumerate who has an account.
    Result<UserAccount> signIn(const std::string& username, const std::string& password);
    void signOut();

    /// Creates an account with a temporary password the operator must replace
    /// before doing anything else — so no credential an administrator knows
    /// stays attached to another person's audited actions.
    Result<UserAccount> createAccount(const std::string& username, const std::string& displayName,
                                      UserRole role, const std::string& temporaryPassword);

    /// Changes a password after re-checking the current one. Requiring it again
    /// is what stops an unattended unlocked workstation becoming a permanent
    /// account takeover.
    Status changePassword(const std::string& userId, const std::string& currentPassword,
                          const std::string& newPassword);

    /// Administrator reset: no current password, and the account must change it
    /// at next sign-in.
    Status resetPassword(const std::string& userId, const std::string& temporaryPassword);

    Status setRole(const std::string& userId, UserRole role);
    Status setActive(const std::string& userId, bool active);
    Result<std::vector<UserAccount>> listAccounts(bool includeInactive = false);

    /// True when the signed-in account is still using a credential an
    /// administrator issued and must replace it.
    bool mustChangePassword() const { return mustChangePassword_; }

private:
    Result<UserAccount> establishSession(const StoredAccount& stored);

    std::shared_ptr<Database> database_;
    std::shared_ptr<AuditService> audit_;
    std::unique_ptr<UserRepository> users_;
    bool mustChangePassword_ = false;
};

}  // namespace trace
