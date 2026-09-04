#include "core/database/database.h"

#include <sqlite3.h>

#include <utility>

#include "core/common/logging.h"

namespace trace {
namespace {

constexpr const char* kComponent = "database";

Status sqliteError(sqlite3* db, const std::string& what) {
    const std::string detail = db != nullptr ? sqlite3_errmsg(db) : "no connection";
    logError(kComponent, what, JsonValue::object().set("detail", detail));
    return Status::failure(ErrorCode::DatabaseError, what, detail);
}

}  // namespace

// ------------------------------------------------------------------ Statement

Statement::~Statement() {
    if (stmt_ != nullptr) sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& other) noexcept : db_(other.db_), stmt_(other.stmt_) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_ != nullptr) sqlite3_finalize(stmt_);
        db_ = other.db_;
        stmt_ = other.stmt_;
        other.db_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

Statement& Statement::bind(int index, std::nullptr_t) {
    sqlite3_bind_null(stmt_, index);
    return *this;
}
Statement& Statement::bind(int index, std::int64_t value) {
    sqlite3_bind_int64(stmt_, index, value);
    return *this;
}
Statement& Statement::bind(int index, int value) {
    sqlite3_bind_int(stmt_, index, value);
    return *this;
}
Statement& Statement::bind(int index, double value) {
    sqlite3_bind_double(stmt_, index, value);
    return *this;
}
Statement& Statement::bind(int index, bool value) {
    sqlite3_bind_int(stmt_, index, value ? 1 : 0);
    return *this;
}
Statement& Statement::bind(int index, const std::string& value) {
    sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    return *this;
}
Statement& Statement::bind(int index, const char* value) {
    if (value == nullptr) return bind(index, nullptr);
    sqlite3_bind_text(stmt_, index, value, -1, SQLITE_TRANSIENT);
    return *this;
}
Statement& Statement::bindOptional(int index, const std::optional<std::int64_t>& value) {
    return value ? bind(index, *value) : bind(index, nullptr);
}
Statement& Statement::bindOptional(int index, const std::optional<double>& value) {
    return value ? bind(index, *value) : bind(index, nullptr);
}
Statement& Statement::bindOptional(int index, const std::optional<std::string>& value) {
    return value ? bind(index, *value) : bind(index, nullptr);
}

Result<bool> Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return Result<bool>::success(true);
    if (rc == SQLITE_DONE) return Result<bool>::success(false);
    return Result<bool>::failure(ErrorCode::DatabaseError, "Database step failed",
                                 db_ != nullptr ? sqlite3_errmsg(db_) : "unknown");
}

Status Statement::run() {
    auto stepped = step();
    if (!stepped) return Status(stepped.error());
    while (stepped.ok() && stepped.value()) {
        stepped = step();
        if (!stepped) return Status(stepped.error());
    }
    return Status::success();
}

Status Statement::reset() {
    if (sqlite3_reset(stmt_) != SQLITE_OK) return sqliteError(db_, "Statement reset failed");
    sqlite3_clear_bindings(stmt_);
    return Status::success();
}

bool Statement::isNull(int column) const {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}
std::int64_t Statement::columnInt64(int column) const { return sqlite3_column_int64(stmt_, column); }
int Statement::columnInt(int column) const { return sqlite3_column_int(stmt_, column); }
double Statement::columnDouble(int column) const { return sqlite3_column_double(stmt_, column); }

std::string Statement::columnText(int column) const {
    const auto* text = sqlite3_column_text(stmt_, column);
    if (text == nullptr) return {};
    const int bytes = sqlite3_column_bytes(stmt_, column);
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

std::optional<std::int64_t> Statement::columnOptionalInt64(int column) const {
    if (isNull(column)) return std::nullopt;
    return columnInt64(column);
}
std::optional<double> Statement::columnOptionalDouble(int column) const {
    if (isNull(column)) return std::nullopt;
    return columnDouble(column);
}
std::optional<std::string> Statement::columnOptionalText(int column) const {
    if (isNull(column)) return std::nullopt;
    return columnText(column);
}

int Statement::columnCount() const { return sqlite3_column_count(stmt_); }

std::string Statement::columnName(int column) const {
    const char* name = sqlite3_column_name(stmt_, column);
    return name != nullptr ? name : "";
}

// ------------------------------------------------------------------- Database

Database::Database(sqlite3* handle, std::filesystem::path path)
    : handle_(handle), path_(std::move(path)) {}

Database::~Database() {
    if (handle_ != nullptr) {
        sqlite3_close(handle_);
        handle_ = nullptr;
    }
}

Result<std::shared_ptr<Database>> Database::open(const std::filesystem::path& path) {
    return openInternal(path, false);
}

Result<std::shared_ptr<Database>> Database::openInMemory() {
    return openInternal(":memory:", true);
}

Result<std::shared_ptr<Database>> Database::openInternal(const std::filesystem::path& path,
                                                        bool inMemory) {
    using ResultType = Result<std::shared_ptr<Database>>;

    if (!inMemory) {
        std::error_code ec;
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return ResultType::failure(ErrorCode::IoError,
                                           "Unable to create database directory: " + parent.string(),
                                           ec.message());
            }
        }
    }

    sqlite3* handle = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    const int rc = sqlite3_open_v2(path.string().c_str(), &handle, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string detail = handle != nullptr ? sqlite3_errmsg(handle) : sqlite3_errstr(rc);
        if (handle != nullptr) sqlite3_close(handle);
        return ResultType::failure(ErrorCode::DatabaseError,
                                   "Unable to open case database: " + path.string(), detail);
    }

    auto database = std::shared_ptr<Database>(new Database(handle, path));

    // Durability and referential integrity are worth more than throughput here:
    // this file is the index of the evidence, and a torn write loses provenance.
    const char* pragmas =
        "PRAGMA foreign_keys = ON;"
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = FULL;"
        "PRAGMA busy_timeout = 5000;"
        "PRAGMA temp_store = MEMORY;";
    if (auto status = database->execute(pragmas); !status) {
        return ResultType(status.error());
    }

    logInfo(kComponent, "Database opened",
            JsonValue::object().set("path", inMemory ? std::string(":memory:") : path.string()));
    return ResultType::success(std::move(database));
}

Status Database::execute(const std::string& sql) {
    auto guard = lockGuard();
    char* message = nullptr;
    const int rc = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const std::string detail = message != nullptr ? message : sqlite3_errmsg(handle_);
        if (message != nullptr) sqlite3_free(message);
        logError(kComponent, "SQL execution failed", JsonValue::object().set("detail", detail));
        return Status::failure(ErrorCode::DatabaseError, "SQL execution failed", detail);
    }
    if (message != nullptr) sqlite3_free(message);
    return Status::success();
}

Result<Statement> Database::prepare(const std::string& sql) {
    auto guard = lockGuard();
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(handle_, sql.c_str(), static_cast<int>(sql.size()), &stmt,
                                      nullptr);
    if (rc != SQLITE_OK) {
        const std::string detail = sqlite3_errmsg(handle_);
        logError(kComponent, "Statement preparation failed",
                 JsonValue::object().set("detail", detail).set("sql", sql.substr(0, 200)));
        return Result<Statement>::failure(ErrorCode::DatabaseError, "Statement preparation failed",
                                          detail);
    }
    return Result<Statement>::success(Statement(handle_, stmt));
}

Result<std::int64_t> Database::queryInt64(const std::string& sql) {
    auto guard = lockGuard();
    auto prepared = prepare(sql);
    if (!prepared) return Result<std::int64_t>(prepared.error());
    Statement stmt = prepared.take();
    auto stepped = stmt.step();
    if (!stepped) return Result<std::int64_t>(stepped.error());
    if (!stepped.value()) {
        return Result<std::int64_t>::failure(ErrorCode::NotFound, "Query returned no rows");
    }
    return Result<std::int64_t>::success(stmt.columnInt64(0));
}

Result<std::optional<std::string>> Database::queryText(const std::string& sql) {
    using ResultType = Result<std::optional<std::string>>;
    auto guard = lockGuard();
    auto prepared = prepare(sql);
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(stmt.columnOptionalText(0));
}

std::int64_t Database::lastInsertRowId() const { return sqlite3_last_insert_rowid(handle_); }
int Database::changes() const { return sqlite3_changes(handle_); }

std::string Database::lastErrorMessage() const {
    return handle_ != nullptr ? sqlite3_errmsg(handle_) : "no connection";
}

// ---------------------------------------------------------------- Transaction

Transaction::Transaction(Database& database) : database_(database), lock_(database.lockGuard()) {}

Transaction::~Transaction() {
    if (active_) rollback();
}

namespace {

std::string savepointName(int depth) { return "trace_savepoint_" + std::to_string(depth); }

}  // namespace

Status Transaction::begin() {
    if (active_) return Status::failure(ErrorCode::Internal, "Transaction already active");

    depth_ = database_.transactionDepth_;
    const std::string sql =
        depth_ == 0 ? std::string("BEGIN IMMEDIATE;") : "SAVEPOINT " + savepointName(depth_) + ";";
    if (auto status = database_.execute(sql); !status) return status;

    database_.transactionDepth_ = depth_ + 1;
    active_ = true;
    return Status::success();
}

Status Transaction::commit() {
    if (!active_) return Status::failure(ErrorCode::Internal, "No active transaction to commit");

    const std::string sql =
        depth_ == 0 ? std::string("COMMIT;") : "RELEASE " + savepointName(depth_) + ";";
    if (auto status = database_.execute(sql); !status) return status;

    database_.transactionDepth_ = depth_;
    active_ = false;
    return Status::success();
}

void Transaction::rollback() {
    if (!active_) return;

    const std::string sql = depth_ == 0
                                ? std::string("ROLLBACK;")
                                : "ROLLBACK TO " + savepointName(depth_) + "; RELEASE " +
                                      savepointName(depth_) + ";";
    if (auto status = database_.execute(sql); !status) {
        logError(kComponent, "Rollback failed",
                 JsonValue::object().set("detail", status.error().detail()));
    }
    database_.transactionDepth_ = depth_;
    active_ = false;
}

}  // namespace trace
