#pragma once

#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"

namespace trace {

/// A single forward-only schema migration. `sql` is embedded into the binary at
/// build time from migrations/*.sql, so a deployed TRACE build can always
/// create or upgrade a database without shipping loose SQL files alongside it.
struct Migration {
    int version = 0;
    std::string name;
    std::string sql;
};

/// Migrations compiled into this build, ordered by version.
const std::vector<Migration>& embeddedMigrations();

/// A row of the schema_migrations table.
struct MigrationRecord {
    int version = 0;
    std::string name;
    std::string checksum;
    std::string appliedAt;
};

struct MigrationReport {
    int previousVersion = 0;
    int currentVersion = 0;
    std::vector<int> appliedVersions;
};

/// Applies pending migrations and guards against schema drift.
///
/// Each applied migration's SHA-256 is recorded. On every startup the recorded
/// digests are compared with the migrations compiled into the running binary:
/// if a migration file was edited after it had been applied, the database and
/// the code no longer agree and TRACE refuses to continue rather than operating
/// on a schema it cannot describe.
class MigrationRunner {
public:
    static Status ensureMigrationTable(Database& database);
    static Result<int> currentVersion(Database& database);
    static Result<std::vector<MigrationRecord>> appliedMigrations(Database& database);
    static Result<MigrationReport> applyAll(Database& database,
                                            const std::vector<Migration>& migrations = embeddedMigrations());
};

}  // namespace trace
