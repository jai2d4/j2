#include "core/services/auth_service.h"

#include <utility>

#include "core/common/logging.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"

namespace trace {
namespace {

constexpr const char* kComponent = "auth";

/// One message for every way a sign-in can fail to match.
///
/// Saying "no such user" would turn the prompt into a directory of who has an
/// account, which is useful to an attacker and to nobody else.
constexpr const char* kRejected = "That username and password do not match an account.";

/// Verified against when the username does not exist.
///
/// Returning early would make a missing account measurably faster than a wrong
/// password, and that difference is enough to enumerate usernames over a few
/// hundred attempts. Doing the same work for both costs one PBKDF2 evaluation.
const password::StoredPassword& decoyCredential() {
    static const password::StoredPassword decoy = [] {
        password::StoredPassword stored;
        stored.algorithm = password::kAlgorithmPbkdf2Sha256;
        stored.iterations = password::kDefaultIterations;
        stored.saltHex = std::string(password::kSaltBytes * 2, '0');
        stored.hashHex = std::string(password::kKeyBytes * 2, '0');
        return stored;
    }();
    return decoy;
}

}  // namespace

AuthService::AuthService(std::shared_ptr<Database> database, std::shared_ptr<AuditService> audit)
    : database_(std::move(database)),
      audit_(std::move(audit)),
      users_(std::make_unique<UserRepository>(database_)) {}

Result<bool> AuthService::needsFirstRunSetup() {
    auto usable = users_->countUsableAccounts();
    if (!usable) return Result<bool>(usable.error());
    return Result<bool>::success(usable.value() == 0);
}

Result<UserAccount> AuthService::createFirstAdministrator(const std::string& username,
                                                          const std::string& displayName,
                                                          const std::string& plaintext) {
    using ResultType = Result<UserAccount>;

    // Checked inside the same call rather than trusted from the caller: this is
    // the one path that mints an administrator without authenticating, so it has
    // to close itself the moment an account exists.
    auto needed = needsFirstRunSetup();
    if (!needed) return ResultType(needed.error());
    if (!needed.value()) {
        return ResultType::failure(ErrorCode::PermissionDenied,
                                   "An account already exists. Sign in instead.");
    }
    return createAccount(username, displayName, UserRole::Administrator, plaintext);
}

Result<UserAccount> AuthService::createAccount(const std::string& username,
                                               const std::string& displayName, UserRole role,
                                               const std::string& plaintext) {
    using ResultType = Result<UserAccount>;

    if (username.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "A username is required");
    }

    auto existing = users_->findByUsername(username);
    if (!existing) return ResultType(existing.error());
    if (existing.value()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "An account with that username already exists");
    }

    auto credential = password::hash(plaintext);
    if (!credential) return ResultType(credential.error());

    StoredAccount stored;
    stored.account.id = generateUuid();
    stored.account.username = username;
    stored.account.displayName = displayName.empty() ? username : displayName;
    stored.account.role = role;
    stored.account.active = true;
    stored.account.createdAt = nowIso8601Utc();
    stored.credential = credential.take();
    stored.passwordChangedAt = stored.account.createdAt;
    // An account created here starts with a password somebody else chose, so the
    // holder must replace it before their actions are attributed to them. The
    // first administrator is the exception: they typed their own during setup,
    // and createFirstAdministrator is the only caller that reaches this with no
    // other account in existence.
    auto usable = users_->countUsableAccounts();
    stored.mustChangePassword = !(usable.ok() && usable.value() == 0);

    if (auto status = users_->insertWithCredential(stored); !status) return ResultType(status.error());

    AuditRecord record;
    record.action = AuditAction::AccountCreated;
    record.description = "Account created for " + username;
    record.details = JsonValue::object()
                         .set("username", username)
                         .set("role", std::string(toString(role)));
    audit_->record(record);

    return ResultType::success(stored.account);
}

Result<UserAccount> AuthService::signIn(const std::string& username, const std::string& plaintext) {
    using ResultType = Result<UserAccount>;

    auto found = users_->findStoredByUsername(username);
    if (!found) return ResultType(found.error());

    // Nothing about the outcome is decided before the credential check runs:
    // a missing account, an inactive one and one with no password all verify
    // against the decoy so they cost the same as a real wrong password.
    const bool exists = found.value().has_value();
    const StoredAccount stored = exists ? *found.value() : StoredAccount{};
    const auto& credential =
        (exists && stored.hasCredential()) ? *stored.credential : decoyCredential();

    const bool matched = password::verify(plaintext, credential);
    const bool usable = exists && stored.account.active && stored.hasCredential();

    // A lockout still in force refuses even a correct password. Checked after
    // verification so a locked account does not answer faster than a wrong one.
    bool locked = false;
    if (usable && stored.lockedUntil) {
        locked = nowIso8601Utc() < *stored.lockedUntil;
    }

    if (!usable || !matched || locked) {
        if (usable && !locked) {
            const int attempts = stored.failedAttempts + 1;
            std::optional<std::string> lockedUntil;
            if (attempts >= kMaxFailedAttempts) {
                lockedUntil = iso8601UtcPlusMinutes(kLockoutMinutes);
            }
            users_->recordFailedAttempt(stored.account.id, attempts, lockedUntil);

            if (lockedUntil) {
                AuditRecord record;
                record.action = AuditAction::AccountLocked;
                record.outcome = "warning";
                record.description = "Account locked after " + std::to_string(attempts) +
                                     " failed sign-in attempts: " + username;
                record.details = JsonValue::object()
                                     .set("username", username)
                                     .set("locked_until", *lockedUntil);
                audit_->record(record);
            }
        }

        AuditRecord record;
        record.action = AuditAction::SignInFailed;
        record.outcome = "failure";
        // The username is recorded because a run of attempts against one name is
        // the thing worth seeing. The password is not, in any form: not the
        // plaintext, not a hash of it, not its length.
        record.description = "Sign-in failed for " + username;
        record.details = JsonValue::object()
                             .set("username", username)
                             .set("reason", locked ? "locked" : (exists ? "mismatch" : "unknown"));
        audit_->record(record);

        logInfo(kComponent, "Sign-in rejected", JsonValue::object().set("username", username));

        if (locked) {
            return ResultType::failure(
                ErrorCode::PermissionDenied,
                "This account is temporarily locked after repeated failed attempts. Try again "
                "in a few minutes.");
        }
        return ResultType::failure(ErrorCode::PermissionDenied, kRejected);
    }

    return establishSession(stored);
}

Result<UserAccount> AuthService::establishSession(const StoredAccount& stored) {
    using ResultType = Result<UserAccount>;

    const std::string at = nowIso8601Utc();
    users_->recordSuccessfulLogin(stored.account.id, at);

    UserAccount account = stored.account;
    account.lastSeenAt = at;
    UserContext::current().setAccount(account);
    mustChangePassword_ = stored.mustChangePassword;

    AuditRecord record;
    record.action = AuditAction::SignInSucceeded;
    record.description = "Signed in: " + account.username;
    record.details = JsonValue::object()
                         .set("username", account.username)
                         .set("role", std::string(toString(account.role)));
    audit_->record(record);

    return ResultType::success(std::move(account));
}

void AuthService::signOut() {
    const auto account = UserContext::current().account();
    if (!account.id.empty()) {
        AuditRecord record;
        record.action = AuditAction::SignedOut;
        record.description = "Signed out: " + account.username;
        record.details = JsonValue::object().set("username", account.username);
        audit_->record(record);
    }
    UserContext::current().setAccount(UserAccount{});
    mustChangePassword_ = false;
}

Status AuthService::changePassword(const std::string& userId, const std::string& currentPlaintext,
                                   const std::string& newPlaintext) {
    auto found = users_->findStoredById(userId);
    if (!found) return Status(found.error());
    if (!found.value()) return Status::failure(ErrorCode::NotFound, "No such account");

    const StoredAccount stored = *found.value();
    if (!stored.hasCredential() || !password::verify(currentPlaintext, *stored.credential)) {
        AuditRecord record;
        record.action = AuditAction::SignInFailed;
        record.outcome = "failure";
        record.description = "Password change refused: current password did not match, for " +
                             stored.account.username;
        audit_->record(record);
        return Status::failure(ErrorCode::PermissionDenied,
                               "The current password is not correct.");
    }

    auto credential = password::hash(newPlaintext);
    if (!credential) return Status(credential.error());

    if (auto status = users_->setCredential(userId, credential.value(), nowIso8601Utc(), false);
        !status) {
        return status;
    }
    mustChangePassword_ = false;

    AuditRecord record;
    record.action = AuditAction::PasswordChanged;
    record.description = "Password changed for " + stored.account.username;
    record.details = JsonValue::object().set("username", stored.account.username);
    audit_->record(record);
    return Status::success();
}

Status AuthService::resetPassword(const std::string& userId, const std::string& temporaryPlaintext) {
    if (!UserContext::current().can(Permission::ManageUsers)) {
        return Status::failure(ErrorCode::PermissionDenied,
                               "Resetting another account's password requires the Administrator "
                               "role.");
    }

    auto found = users_->findStoredById(userId);
    if (!found) return Status(found.error());
    if (!found.value()) return Status::failure(ErrorCode::NotFound, "No such account");

    auto credential = password::hash(temporaryPlaintext);
    if (!credential) return Status(credential.error());

    // mustChange is true: an administrator now knows this password, so it cannot
    // be the one the account's audited actions are attributed to.
    if (auto status = users_->setCredential(userId, credential.value(), nowIso8601Utc(), true);
        !status) {
        return status;
    }

    AuditRecord record;
    record.action = AuditAction::PasswordReset;
    record.outcome = "warning";
    record.description = "Password reset by an administrator for " + found.value()->account.username;
    record.details = JsonValue::object()
                         .set("username", found.value()->account.username)
                         .set("reset_by", UserContext::current().actorName());
    audit_->record(record);
    return Status::success();
}

Status AuthService::setRole(const std::string& userId, UserRole role) {
    if (!UserContext::current().can(Permission::ManageUsers)) {
        return Status::failure(ErrorCode::PermissionDenied,
                               "Changing a role requires the Administrator role.");
    }
    if (auto status = users_->setRole(userId, role); !status) return status;

    AuditRecord record;
    record.action = AuditAction::AccountRoleChanged;
    record.description = "Role changed to " + std::string(toString(role));
    record.details = JsonValue::object()
                         .set("user_id", userId)
                         .set("role", std::string(toString(role)))
                         .set("changed_by", UserContext::current().actorName());
    audit_->record(record);
    return Status::success();
}

Status AuthService::setActive(const std::string& userId, bool active) {
    if (!UserContext::current().can(Permission::ManageUsers)) {
        return Status::failure(ErrorCode::PermissionDenied,
                               "Deactivating an account requires the Administrator role.");
    }
    if (auto status = users_->setActive(userId, active); !status) return status;

    AuditRecord record;
    record.action = AuditAction::AccountDeactivated;
    record.description = active ? "Account reactivated" : "Account deactivated";
    record.details = JsonValue::object()
                         .set("user_id", userId)
                         .set("active", active)
                         .set("changed_by", UserContext::current().actorName());
    audit_->record(record);
    return Status::success();
}

Result<std::vector<UserAccount>> AuthService::listAccounts(bool includeInactive) {
    if (includeInactive) return users_->list();
    auto all = users_->list();
    if (!all) return all;
    std::vector<UserAccount> active;
    for (auto& account : all.value()) {
        if (account.active) active.push_back(account);
    }
    return Result<std::vector<UserAccount>>::success(std::move(active));
}

}  // namespace trace
