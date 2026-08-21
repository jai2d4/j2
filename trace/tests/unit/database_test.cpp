// Schema migrations, the append-only audit guarantee and drift detection.
#include <gtest/gtest.h>

#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/database/database.h"
#include "core/database/migrations.h"
#include "core/security/sha256.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

std::shared_ptr<Database> freshDatabase() {
    auto opened = Database::openInMemory();
    EXPECT_TRUE(opened.ok()) << opened.error().toString();
    return opened.take();
}

TEST(Migrations, ApplyEverySchemaVersionOnce) {
    auto database = freshDatabase();

    auto report = MigrationRunner::applyAll(*database);
    ASSERT_TRUE(report.ok()) << report.error().toString();
    EXPECT_EQ(report.value().previousVersion, 0);
    EXPECT_GE(report.value().currentVersion, 2);
    EXPECT_EQ(report.value().appliedVersions.size(), embeddedMigrations().size());

    // Re-running is a no-op, which is what makes startup safe.
    auto second = MigrationRunner::applyAll(*database);
    ASSERT_TRUE(second.ok()) << second.error().toString();
    EXPECT_TRUE(second.value().appliedVersions.empty());
    EXPECT_EQ(second.value().currentVersion, report.value().currentVersion);
}

TEST(Migrations, CreateEveryTableTheApplicationDependsOn) {
    auto database = freshDatabase();
    ASSERT_TRUE(MigrationRunner::applyAll(*database).ok());

    for (const char* table : {"cases", "evidence", "evidence_metadata", "media_streams",
                              "processing_operations", "derived_assets", "bookmarks",
                              "annotations", "audit_events", "application_settings", "users",
                              "schema_migrations"}) {
        auto prepared = database->prepare(
            "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?;");
        ASSERT_TRUE(prepared.ok());
        Statement statement = prepared.take();
        statement.bind(1, std::string(table));
        auto stepped = statement.step();
        ASSERT_TRUE(stepped.ok() && stepped.value());
        EXPECT_EQ(statement.columnInt(0), 1) << "missing table: " << table;
    }
}

TEST(Migrations, RecordChecksumsAndRefuseDriftedSchema) {
    auto database = freshDatabase();
    ASSERT_TRUE(MigrationRunner::applyAll(*database).ok());

    auto applied = MigrationRunner::appliedMigrations(*database);
    ASSERT_TRUE(applied.ok());
    ASSERT_FALSE(applied.value().empty());
    EXPECT_EQ(applied.value().front().checksum, Sha256::hash(embeddedMigrations().front().sql));

    // Simulate someone editing a migration that has already been applied.
    std::vector<Migration> tampered = embeddedMigrations();
    tampered.front().sql += "\n-- edited after the fact\n";
    auto rerun = MigrationRunner::applyAll(*database, tampered);
    ASSERT_FALSE(rerun.ok());
    EXPECT_EQ(rerun.error().code(), ErrorCode::DatabaseError);
    EXPECT_NE(rerun.error().message().find("drift"), std::string::npos);
}

TEST(Migrations, RefuseDatabaseFromANewerBuild) {
    auto database = freshDatabase();
    ASSERT_TRUE(MigrationRunner::applyAll(*database).ok());
    ASSERT_TRUE(database
                    ->execute("INSERT INTO schema_migrations (version, name, checksum, applied_at) "
                              "VALUES (9999, '9999_future.sql', 'deadbeef', '2030-01-01T00:00:00.000Z');")
                    .ok());

    auto rerun = MigrationRunner::applyAll(*database);
    ASSERT_FALSE(rerun.ok());
    EXPECT_NE(rerun.error().message().find("newer version"), std::string::npos);
}

TEST(AuditTable, RefusesUpdatesAndDeletesAtTheDatabaseLevel) {
    auto database = freshDatabase();
    ASSERT_TRUE(MigrationRunner::applyAll(*database).ok());

    auto prepared = database->prepare(
        "INSERT INTO audit_events (id, sequence, occurred_at, action, actor, description) "
        "VALUES (?, 1, ?, 'case.created', 'tester', 'created');");
    ASSERT_TRUE(prepared.ok());
    Statement insert = prepared.take();
    insert.bind(1, generateUuid()).bind(2, nowIso8601Utc());
    ASSERT_TRUE(insert.run().ok());

    // Both of these must fail: history is not editable, even by TRACE itself.
    auto updated = database->execute("UPDATE audit_events SET description = 'changed';");
    EXPECT_FALSE(updated.ok());
    EXPECT_NE(updated.error().detail().find("append-only"), std::string::npos);

    auto deleted = database->execute("DELETE FROM audit_events;");
    EXPECT_FALSE(deleted.ok());
    EXPECT_NE(deleted.error().detail().find("append-only"), std::string::npos);

    auto count = database->queryInt64("SELECT COUNT(*) FROM audit_events;");
    ASSERT_TRUE(count.ok());
    EXPECT_EQ(count.value(), 1);
}

TEST(Database, EnforcesForeignKeysAndCascades) {
    auto database = freshDatabase();
    ASSERT_TRUE(MigrationRunner::applyAll(*database).ok());

    // Evidence pointing at a case that does not exist must be rejected.
    auto prepared = database->prepare(
        "INSERT INTO evidence (id, case_id, evidence_number, original_filename, stored_filename, "
        "source_path, storage_relpath, file_size, sha256, ingested_at, modified_at) "
        "VALUES (?, 'no-such-case', 'EVD-000001', 'a.mp4', 'a.mp4', '/a.mp4', 'x', 1, 'abc', ?, ?);");
    ASSERT_TRUE(prepared.ok());
    Statement statement = prepared.take();
    const std::string now = nowIso8601Utc();
    statement.bind(1, generateUuid()).bind(2, now).bind(3, now);
    EXPECT_FALSE(statement.run().ok()) << "foreign keys are not being enforced";
}

TEST(Database, TransactionsRollBackCleanly) {
    auto database = freshDatabase();
    ASSERT_TRUE(MigrationRunner::applyAll(*database).ok());

    {
        Transaction transaction(*database);
        ASSERT_TRUE(transaction.begin().ok());
        auto prepared = database->prepare(
            "INSERT INTO cases (id, case_number, title, created_at, modified_at, storage_relpath) "
            "VALUES (?, 'CASE-0001', 'Rolled back', ?, ?, 'cases/x');");
        ASSERT_TRUE(prepared.ok());
        Statement statement = prepared.take();
        const std::string now = nowIso8601Utc();
        statement.bind(1, generateUuid()).bind(2, now).bind(3, now);
        ASSERT_TRUE(statement.run().ok());
        // Destructor rolls back because commit() was never called.
    }

    auto count = database->queryInt64("SELECT COUNT(*) FROM cases;");
    ASSERT_TRUE(count.ok());
    EXPECT_EQ(count.value(), 0);
}

TEST(Database, BindsUserTextSafely) {
    auto database = freshDatabase();
    ASSERT_TRUE(MigrationRunner::applyAll(*database).ok());

    const std::string hostileTitle = "Robert'); DROP TABLE cases;--";
    auto prepared = database->prepare(
        "INSERT INTO cases (id, case_number, title, created_at, modified_at, storage_relpath) "
        "VALUES (?, ?, ?, ?, ?, 'cases/x');");
    ASSERT_TRUE(prepared.ok());
    Statement statement = prepared.take();
    const std::string now = nowIso8601Utc();
    statement.bind(1, generateUuid()).bind(2, std::string("CASE-0001")).bind(3, hostileTitle)
        .bind(4, now).bind(5, now);
    ASSERT_TRUE(statement.run().ok());

    auto stored = database->queryText("SELECT title FROM cases;");
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    EXPECT_EQ(*stored.value(), hostileTitle);
}

}  // namespace
}  // namespace trace
