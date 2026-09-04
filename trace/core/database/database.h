#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/common/result.h"

struct sqlite3;
struct sqlite3_stmt;

namespace trace {

class Database;

/// RAII prepared statement with checked binding and column access.
///
/// Every value that reaches SQLite goes through a bind call: TRACE never
/// concatenates user text into SQL, so a case title containing a quote cannot
/// corrupt the evidence database.
class Statement {
public:
    Statement() = default;
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    /// Parameter indices are 1-based, matching SQLite.
    Statement& bind(int index, std::nullptr_t);
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, int value);
    Statement& bind(int index, double value);
    Statement& bind(int index, bool value);
    Statement& bind(int index, const std::string& value);
    Statement& bind(int index, const char* value);
    /// Binds NULL when the optional is empty — used for "not available" fields.
    Statement& bindOptional(int index, const std::optional<std::int64_t>& value);
    Statement& bindOptional(int index, const std::optional<double>& value);
    Statement& bindOptional(int index, const std::optional<std::string>& value);

    /// Advances the statement. Returns true when a row is available, false when
    /// the statement is done, or an Error on failure.
    Result<bool> step();
    /// Runs a statement that returns no rows.
    Status run();
    Status reset();

    // Column accessors (0-based, matching SQLite).
    bool isNull(int column) const;
    std::int64_t columnInt64(int column) const;
    int columnInt(int column) const;
    double columnDouble(int column) const;
    std::string columnText(int column) const;
    std::optional<std::int64_t> columnOptionalInt64(int column) const;
    std::optional<double> columnOptionalDouble(int column) const;
    std::optional<std::string> columnOptionalText(int column) const;
    int columnCount() const;
    std::string columnName(int column) const;

private:
    friend class Database;
    Statement(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

/// Owning handle to the TRACE case database.
///
/// The connection is serialized behind a recursive mutex so background workers
/// (ingestion, hashing, thumbnailing) and the UI thread can share one database
/// without interleaving statements mid-transaction. Callers that need several
/// statements to be atomic hold `lockGuard()` for the duration.
class Database {
public:
    static Result<std::shared_ptr<Database>> open(const std::filesystem::path& path);
    /// In-memory database, used by the unit suite.
    static Result<std::shared_ptr<Database>> openInMemory();

    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Status execute(const std::string& sql);
    Result<Statement> prepare(const std::string& sql);

    /// Convenience helpers for single-value queries.
    Result<std::int64_t> queryInt64(const std::string& sql);
    Result<std::optional<std::string>> queryText(const std::string& sql);

    std::int64_t lastInsertRowId() const;
    int changes() const;

    const std::filesystem::path& path() const { return path_; }
    sqlite3* handle() const { return handle_; }

    std::unique_lock<std::recursive_mutex> lockGuard() {
        return std::unique_lock<std::recursive_mutex>(mutex_);
    }

    std::string lastErrorMessage() const;

    /// Depth of nested transaction scopes currently open on this connection.
    int transactionDepth() const { return transactionDepth_; }

private:
    friend class Transaction;
    Database(sqlite3* handle, std::filesystem::path path);
    static Result<std::shared_ptr<Database>> openInternal(const std::filesystem::path& path,
                                                          bool inMemory);

    sqlite3* handle_ = nullptr;
    std::filesystem::path path_;
    mutable std::recursive_mutex mutex_;
    int transactionDepth_ = 0;
};

/// Scoped transaction. Rolls back on destruction unless commit() succeeded, so
/// a failure part-way through ingestion can never leave half an evidence record
/// behind.
///
/// Scopes nest: a repository that wraps its own writes in a transaction can be
/// called from inside a service-level transaction, and the inner scope becomes
/// a savepoint. Rolling the outer scope back still discards everything.
class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Status begin();
    Status commit();
    void rollback();
    bool active() const { return active_; }

private:
    Database& database_;
    std::unique_lock<std::recursive_mutex> lock_;
    bool active_ = false;
    int depth_ = 0;
};

}  // namespace trace
