#include "core/repositories/user_repository.h"

#include <utility>

#include "core/common/time_utils.h"

namespace trace {
namespace {

constexpr const char* kColumns =
    "id, username, display_name, role, active, created_at, last_seen_at";

UserAccount readUser(const Statement& stmt) {
    UserAccount account;
    account.id = stmt.columnText(0);
    account.username = stmt.columnText(1);
    account.displayName = stmt.columnText(2);
    account.role = userRoleFromString(stmt.columnText(3));
    account.active = stmt.columnInt(4) != 0;
    account.createdAt = stmt.columnText(5);
    account.lastSeenAt = stmt.columnText(6);
    return account;
}

}  // namespace

UserRepository::UserRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Result<UserAccount> UserRepository::upsert(const UserAccount& account) {
    using ResultType = Result<UserAccount>;
    auto guard = database_->lockGuard();

    auto existing = findByUsername(account.username);
    if (!existing) return ResultType(existing.error());

    if (existing.value().has_value()) {
        auto prepared = database_->prepare(
            "UPDATE users SET display_name = ?, role = ?, active = ?, last_seen_at = ? "
            "WHERE username = ?;");
        if (!prepared) return ResultType(prepared.error());
        Statement stmt = prepared.take();
        stmt.bind(1, account.displayName)
            .bind(2, std::string(toString(account.role)))
            .bind(3, account.active)
            .bind(4, nowIso8601Utc())
            .bind(5, account.username);
        if (auto status = stmt.run(); !status) return ResultType(status.error());

        UserAccount stored = *existing.value();
        stored.displayName = account.displayName;
        stored.role = account.role;
        stored.active = account.active;
        stored.lastSeenAt = nowIso8601Utc();
        return ResultType::success(std::move(stored));
    }

    auto prepared = database_->prepare(std::string("INSERT INTO users (") + kColumns +
                                       ") VALUES (?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return ResultType(prepared.error());
    UserAccount stored = account;
    if (stored.createdAt.empty()) stored.createdAt = nowIso8601Utc();
    stored.lastSeenAt = nowIso8601Utc();

    Statement stmt = prepared.take();
    stmt.bind(1, stored.id)
        .bind(2, stored.username)
        .bind(3, stored.displayName)
        .bind(4, std::string(toString(stored.role)))
        .bind(5, stored.active)
        .bind(6, stored.createdAt)
        .bind(7, stored.lastSeenAt);
    if (auto status = stmt.run(); !status) return ResultType(status.error());
    return ResultType::success(std::move(stored));
}

Result<std::optional<UserAccount>> UserRepository::findByUsername(const std::string& username) {
    using ResultType = Result<std::optional<UserAccount>>;
    auto prepared =
        database_->prepare(std::string("SELECT ") + kColumns + " FROM users WHERE username = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, username);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readUser(stmt));
}

Result<std::vector<UserAccount>> UserRepository::list() {
    using ResultType = Result<std::vector<UserAccount>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kColumns + " FROM users ORDER BY username;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();

    std::vector<UserAccount> accounts;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        accounts.push_back(readUser(stmt));
    }
    return ResultType::success(std::move(accounts));
}

// ---------------------------------------------------------------- credentials

namespace {

constexpr const char* kStoredColumns =
    "id, username, display_name, role, active, created_at, last_seen_at, "
    "password_hash, password_salt, password_algorithm, password_iterations, "
    "password_changed_at, must_change_password, failed_attempts, locked_until, last_login_at";

StoredAccount readStored(const Statement& stmt) {
    StoredAccount stored;
    stored.account = readUser(stmt);

    // A credential is reconstructed only when every part of it is present. A row
    // carrying a hash but no algorithm is treated as having no credential at all
    // rather than being verified under an assumed one.
    const auto hash = stmt.columnOptionalText(7);
    const auto salt = stmt.columnOptionalText(8);
    const auto algorithm = stmt.columnOptionalText(9);
    const auto iterations = stmt.columnOptionalInt64(10);
    if (hash && salt && algorithm && iterations && *iterations > 0) {
        password::StoredPassword credential;
        credential.hashHex = *hash;
        credential.saltHex = *salt;
        credential.algorithm = *algorithm;
        credential.iterations = static_cast<std::uint32_t>(*iterations);
        stored.credential = credential;
    }

    stored.passwordChangedAt = stmt.columnOptionalText(11);
    stored.mustChangePassword = stmt.columnInt64(12) != 0;
    stored.failedAttempts = static_cast<int>(stmt.columnInt64(13));
    stored.lockedUntil = stmt.columnOptionalText(14);
    stored.lastLoginAt = stmt.columnOptionalText(15);
    return stored;
}

}  // namespace

Status UserRepository::insertWithCredential(const StoredAccount& stored) {
    auto prepared = database_->prepare(
        "INSERT INTO users (id, username, display_name, role, active, created_at, "
        "password_hash, password_salt, password_algorithm, password_iterations, "
        "password_changed_at, must_change_password) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());

    std::optional<std::string> hash;
    std::optional<std::string> salt;
    std::optional<std::string> algorithm;
    std::optional<std::int64_t> iterations;
    if (stored.credential) {
        hash = stored.credential->hashHex;
        salt = stored.credential->saltHex;
        algorithm = stored.credential->algorithm;
        iterations = static_cast<std::int64_t>(stored.credential->iterations);
    }

    Statement stmt = prepared.take();
    stmt.bind(1, stored.account.id)
        .bind(2, stored.account.username)
        .bind(3, stored.account.displayName)
        .bind(4, std::string(toString(stored.account.role)))
        .bind(5, stored.account.active)
        .bind(6, stored.account.createdAt)
        .bindOptional(7, hash)
        .bindOptional(8, salt)
        .bindOptional(9, algorithm)
        .bindOptional(10, iterations)
        .bindOptional(11, stored.passwordChangedAt)
        .bind(12, stored.mustChangePassword ? 1 : 0);
    return stmt.run();
}

Result<std::optional<StoredAccount>> UserRepository::findStoredByUsername(
    const std::string& username) {
    using ResultType = Result<std::optional<StoredAccount>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kStoredColumns +
                                       " FROM users WHERE username = ? LIMIT 1;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, username);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readStored(stmt));
}

Result<std::optional<StoredAccount>> UserRepository::findStoredById(const std::string& id) {
    using ResultType = Result<std::optional<StoredAccount>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kStoredColumns +
                                       " FROM users WHERE id = ? LIMIT 1;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, id);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readStored(stmt));
}

Result<std::int64_t> UserRepository::countUsableAccounts() {
    using ResultType = Result<std::int64_t>;
    auto prepared = database_->prepare(
        "SELECT COUNT(*) FROM users WHERE active = 1 AND password_hash IS NOT NULL "
        "AND password_salt IS NOT NULL AND password_algorithm IS NOT NULL;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(0);
    return ResultType::success(stmt.columnInt64(0));
}

Status UserRepository::setCredential(const std::string& userId,
                                     const password::StoredPassword& credential,
                                     const std::string& changedAt, bool mustChange) {
    // Setting a credential also clears any lockout. Someone who has just proved
    // they can set the password is not the attacker the lockout was guarding
    // against, and leaving them locked out would be a denial of service.
    auto prepared = database_->prepare(
        "UPDATE users SET password_hash = ?, password_salt = ?, password_algorithm = ?, "
        "password_iterations = ?, password_changed_at = ?, must_change_password = ?, "
        "failed_attempts = 0, locked_until = NULL WHERE id = ?;");
    if (!prepared) return Status(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, credential.hashHex)
        .bind(2, credential.saltHex)
        .bind(3, credential.algorithm)
        .bind(4, static_cast<std::int64_t>(credential.iterations))
        .bind(5, changedAt)
        .bind(6, mustChange ? 1 : 0)
        .bind(7, userId);
    return stmt.run();
}

Status UserRepository::setRole(const std::string& userId, UserRole role) {
    auto prepared = database_->prepare("UPDATE users SET role = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, std::string(toString(role))).bind(2, userId);
    return stmt.run();
}

Status UserRepository::setActive(const std::string& userId, bool active) {
    auto prepared = database_->prepare("UPDATE users SET active = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, active).bind(2, userId);
    return stmt.run();
}

Status UserRepository::recordFailedAttempt(const std::string& userId, int attempts,
                                           const std::optional<std::string>& lockedUntil) {
    auto prepared =
        database_->prepare("UPDATE users SET failed_attempts = ?, locked_until = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, static_cast<std::int64_t>(attempts)).bindOptional(2, lockedUntil).bind(3, userId);
    return stmt.run();
}

Status UserRepository::recordSuccessfulLogin(const std::string& userId, const std::string& at) {
    auto prepared = database_->prepare(
        "UPDATE users SET failed_attempts = 0, locked_until = NULL, last_login_at = ?, "
        "last_seen_at = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, at).bind(2, at).bind(3, userId);
    return stmt.run();
}

}  // namespace trace
