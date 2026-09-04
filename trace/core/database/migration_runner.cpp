#include "core/database/migrations.h"

#include <algorithm>

#include "core/common/logging.h"
#include "core/common/time_utils.h"
#include "core/security/sha256.h"

namespace trace {
namespace {
constexpr const char* kComponent = "migrations";
}

Status MigrationRunner::ensureMigrationTable(Database& database) {
    return database.execute(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "  version    INTEGER PRIMARY KEY NOT NULL,"
        "  name       TEXT NOT NULL,"
        "  checksum   TEXT NOT NULL,"
        "  applied_at TEXT NOT NULL"
        ");");
}

Result<int> MigrationRunner::currentVersion(Database& database) {
    if (auto status = ensureMigrationTable(database); !status) return Result<int>(status.error());
    auto value = database.queryInt64("SELECT COALESCE(MAX(version), 0) FROM schema_migrations;");
    if (!value) return Result<int>(value.error());
    return Result<int>::success(static_cast<int>(value.value()));
}

Result<std::vector<MigrationRecord>> MigrationRunner::appliedMigrations(Database& database) {
    using ResultType = Result<std::vector<MigrationRecord>>;
    if (auto status = ensureMigrationTable(database); !status) return ResultType(status.error());

    auto prepared = database.prepare(
        "SELECT version, name, checksum, applied_at FROM schema_migrations ORDER BY version;");
    if (!prepared) return ResultType(prepared.error());

    Statement stmt = prepared.take();
    std::vector<MigrationRecord> records;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        MigrationRecord record;
        record.version = stmt.columnInt(0);
        record.name = stmt.columnText(1);
        record.checksum = stmt.columnText(2);
        record.appliedAt = stmt.columnText(3);
        records.push_back(std::move(record));
    }
    return ResultType::success(std::move(records));
}

Result<MigrationReport> MigrationRunner::applyAll(Database& database,
                                                  const std::vector<Migration>& migrations) {
    using ResultType = Result<MigrationReport>;

    auto guard = database.lockGuard();
    if (auto status = ensureMigrationTable(database); !status) return ResultType(status.error());

    auto appliedResult = appliedMigrations(database);
    if (!appliedResult) return ResultType(appliedResult.error());
    const auto applied = appliedResult.take();

    std::vector<Migration> ordered = migrations;
    std::sort(ordered.begin(), ordered.end(),
              [](const Migration& a, const Migration& b) { return a.version < b.version; });

    // Drift check: an already-applied migration must still hash to what the
    // database recorded when it ran.
    for (const auto& record : applied) {
        const auto it = std::find_if(ordered.begin(), ordered.end(), [&](const Migration& m) {
            return m.version == record.version;
        });
        if (it == ordered.end()) {
            return ResultType::failure(
                ErrorCode::DatabaseError,
                "This database was created by a newer version of TRACE",
                "schema version " + std::to_string(record.version) +
                    " (" + record.name + ") is not known to this build");
        }
        const std::string checksum = Sha256::hash(it->sql);
        if (checksum != record.checksum) {
            return ResultType::failure(
                ErrorCode::DatabaseError, "Database schema drift detected",
                "migration " + record.name + " no longer matches the version applied on " +
                    record.appliedAt);
        }
    }

    MigrationReport report;
    report.previousVersion = applied.empty() ? 0 : applied.back().version;
    report.currentVersion = report.previousVersion;

    for (const auto& migration : ordered) {
        if (migration.version <= report.previousVersion) continue;

        logInfo(kComponent, "Applying migration",
                JsonValue::object()
                    .set("version", migration.version)
                    .set("name", migration.name));

        Transaction transaction(database);
        if (auto status = transaction.begin(); !status) return ResultType(status.error());

        if (auto status = database.execute(migration.sql); !status) {
            transaction.rollback();
            return ResultType::failure(ErrorCode::DatabaseError,
                                       "Migration " + migration.name + " failed",
                                       status.error().detail());
        }

        auto prepared = database.prepare(
            "INSERT INTO schema_migrations (version, name, checksum, applied_at) "
            "VALUES (?, ?, ?, ?);");
        if (!prepared) {
            transaction.rollback();
            return ResultType(prepared.error());
        }
        Statement stmt = prepared.take();
        stmt.bind(1, migration.version)
            .bind(2, migration.name)
            .bind(3, Sha256::hash(migration.sql))
            .bind(4, nowIso8601Utc());
        if (auto status = stmt.run(); !status) {
            transaction.rollback();
            return ResultType(status.error());
        }
        if (auto status = transaction.commit(); !status) return ResultType(status.error());

        report.appliedVersions.push_back(migration.version);
        report.currentVersion = migration.version;
    }

    if (!report.appliedVersions.empty()) {
        logInfo(kComponent, "Schema up to date",
                JsonValue::object()
                    .set("from_version", report.previousVersion)
                    .set("to_version", report.currentVersion));
    }
    return ResultType::success(std::move(report));
}

}  // namespace trace
