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

}  // namespace trace
