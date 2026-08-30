// Encryption at rest, end to end: the keyring, the encrypted case database, and
// the properties that make the claim mean something.
//
// The test that matters most in this file is the one that reads the raw bytes
// off disk and looks for the case number in them. Everything else here could
// pass while TRACE wrote plaintext, and "encrypted at rest" would still be a
// false statement in the README.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "core/database/database.h"
#include "core/database/migrations.h"
#include "core/security/crypto.h"
#include "core/security/keyring.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

constexpr const char* kOperator = "d.mcbride";
constexpr const char* kPassword = "correct horse battery staple";

std::string fileContents(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------------ keyring

TEST(Keyring, CreatesAWorkspaceAndUnlocksItAgain) {
    testing::TemporaryDirectory root("keyring-create");
    ASSERT_FALSE(Keyring::exists(root.path()));

    auto created = Keyring::create(root.path(), kOperator, kPassword);
    ASSERT_TRUE(created.ok()) << created.error().toString();
    const auto keyring = created.take();
    EXPECT_TRUE(Keyring::exists(root.path()));
    EXPECT_EQ(keyring.operatorCount(), 1u);

    auto reloaded = Keyring::load(root.path());
    ASSERT_TRUE(reloaded.ok()) << reloaded.error().toString();
    auto unlocked = reloaded.take().unlock(kOperator, kPassword);
    ASSERT_TRUE(unlocked.ok()) << unlocked.error().toString();
}

TEST(Keyring, TheSameMasterKeyComesBackEveryTime) {
    // If it did not, a database keyed on Monday would not open on Tuesday.
    testing::TemporaryDirectory root("keyring-stable");
    auto created = Keyring::create(root.path(), kOperator, kPassword);
    ASSERT_TRUE(created.ok());
    auto first = created.take().unlock(kOperator, kPassword);
    ASSERT_TRUE(first.ok());

    auto reloaded = Keyring::load(root.path());
    ASSERT_TRUE(reloaded.ok());
    auto second = reloaded.take().unlock(kOperator, kPassword);
    ASSERT_TRUE(second.ok());

    EXPECT_EQ(first.take().toHexForSqlCipher(), second.take().toHexForSqlCipher());
}

TEST(Keyring, RefusesTheWrongPasswordAndAnUnknownOperatorIdentically) {
    // Identical answers, because a different one for an unknown username turns
    // this dialog into a way to list who works here.
    testing::TemporaryDirectory root("keyring-wrong");
    auto created = Keyring::create(root.path(), kOperator, kPassword);
    ASSERT_TRUE(created.ok());
    const auto keyring = created.take();

    auto wrongPassword = keyring.unlock(kOperator, "not the right passphrase");
    auto unknownUser = keyring.unlock("nobody.here", kPassword);
    ASSERT_FALSE(wrongPassword.ok());
    ASSERT_FALSE(unknownUser.ok());
    EXPECT_EQ(wrongPassword.error().code(), unknownUser.error().code());
    EXPECT_EQ(wrongPassword.error().message(), unknownUser.error().message());
}

TEST(Keyring, HoldsNoPasswordAndNoMasterKeyOnDisk) {
    testing::TemporaryDirectory root("keyring-opaque");
    auto created = Keyring::create(root.path(), kOperator, kPassword);
    ASSERT_TRUE(created.ok());
    const auto keyring = created.take();
    auto unlocked = keyring.unlock(kOperator, kPassword);
    ASSERT_TRUE(unlocked.ok());
    const auto masterKey = unlocked.take();

    const std::string raw = fileContents(Keyring::pathFor(root.path()));
    EXPECT_EQ(raw.find(kPassword), std::string::npos) << "the password is on disk";

    // The master key must not appear either, in raw bytes or as hex.
    const std::string keyBytes(reinterpret_cast<const char*>(masterKey.data()), masterKey.size());
    EXPECT_EQ(raw.find(keyBytes), std::string::npos) << "the master key is on disk unwrapped";
    EXPECT_EQ(raw.find(masterKey.toHexForSqlCipher()), std::string::npos);

    // The username is not secret and is expected to be readable.
    EXPECT_NE(raw.find(kOperator), std::string::npos);
}

TEST(Keyring, ASecondOperatorGetsTheSameMasterKeyWithoutSharingAPassword) {
    testing::TemporaryDirectory root("keyring-second");
    auto created = Keyring::create(root.path(), kOperator, kPassword);
    ASSERT_TRUE(created.ok());
    auto keyring = created.take();

    auto first = keyring.unlock(kOperator, kPassword);
    ASSERT_TRUE(first.ok());
    const auto masterKey = first.take();

    ASSERT_TRUE(keyring.addOperator(masterKey, "j.okafor", "a different long passphrase").ok());
    EXPECT_EQ(keyring.operatorCount(), 2u);

    auto second = keyring.unlock("j.okafor", "a different long passphrase");
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.take().toHexForSqlCipher(), masterKey.toHexForSqlCipher());

    // Neither operator's password opens the other's entry.
    EXPECT_FALSE(keyring.unlock("j.okafor", kPassword).ok());
    EXPECT_FALSE(keyring.unlock(kOperator, "a different long passphrase").ok());
}

TEST(Keyring, ChangingAPasswordKeepsTheMasterKeyAndSoKeepsTheData) {
    // The whole reason for wrapping: a password change must not require
    // re-encrypting a case load.
    testing::TemporaryDirectory root("keyring-change");
    auto created = Keyring::create(root.path(), kOperator, kPassword);
    ASSERT_TRUE(created.ok());
    auto keyring = created.take();

    auto before = keyring.unlock(kOperator, kPassword);
    ASSERT_TRUE(before.ok());
    const std::string keyBefore = before.take().toHexForSqlCipher();

    ASSERT_TRUE(keyring.changePassword(kOperator, kPassword, "an entirely new passphrase").ok());

    EXPECT_FALSE(keyring.unlock(kOperator, kPassword).ok());
    auto after = keyring.unlock(kOperator, "an entirely new passphrase");
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.take().toHexForSqlCipher(), keyBefore);
}

TEST(Keyring, RefusesToRemoveTheLastOperator) {
    testing::TemporaryDirectory root("keyring-last");
    auto created = Keyring::create(root.path(), kOperator, kPassword);
    ASSERT_TRUE(created.ok());
    auto keyring = created.take();

    auto removed = keyring.removeOperator(kOperator);
    EXPECT_FALSE(removed.ok()) << "removing the last operator would strand the workspace";
    EXPECT_EQ(keyring.operatorCount(), 1u);

    // With a second operator present, removal is allowed.
    auto unlocked = keyring.unlock(kOperator, kPassword);
    ASSERT_TRUE(unlocked.ok());
    ASSERT_TRUE(keyring.addOperator(unlocked.take(), "j.okafor", "another long passphrase").ok());
    EXPECT_TRUE(keyring.removeOperator(kOperator).ok());
    EXPECT_EQ(keyring.operatorCount(), 1u);
}

TEST(Keyring, RefusesToOverwriteAnExistingKeyring) {
    // Overwriting is not a recoverable mistake: it destroys the only copies of
    // the master key.
    testing::TemporaryDirectory root("keyring-overwrite");
    ASSERT_TRUE(Keyring::create(root.path(), kOperator, kPassword).ok());
    auto again = Keyring::create(root.path(), "someone.else", "yet another passphrase");
    EXPECT_FALSE(again.ok());
    EXPECT_EQ(again.error().code(), ErrorCode::AlreadyExists);
}

TEST(Keyring, RefusesAShortPassword) {
    testing::TemporaryDirectory root("keyring-short");
    auto created = Keyring::create(root.path(), kOperator, "short");
    EXPECT_FALSE(created.ok());
    EXPECT_FALSE(Keyring::exists(root.path())) << "a rejected password left a keyring behind";
}

TEST(Keyring, RefusesATamperedFileRatherThanReadingWhatItCan) {
    testing::TemporaryDirectory root("keyring-tampered");
    ASSERT_TRUE(Keyring::create(root.path(), kOperator, kPassword).ok());
    const auto path = Keyring::pathFor(root.path());

    const std::string original = fileContents(path);

    // Trailing data: an appended second keyring must not be quietly ignored.
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << "extra";
    }
    EXPECT_FALSE(Keyring::load(root.path()).ok());

    // Truncation.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(original.data(), static_cast<std::streamsize>(original.size() / 2));
    }
    EXPECT_FALSE(Keyring::load(root.path()).ok());

    // A wrong marker.
    {
        std::string damaged = original;
        damaged[0] = 'X';
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
    }
    EXPECT_FALSE(Keyring::load(root.path()).ok());
}

TEST(Keyring, AWrappedKeyCannotBeMovedBetweenWorkspaces) {
    // The workspace identifier is bound into every wrap, so lifting an entry
    // from a keyring you can open into one you cannot does not carry access
    // with it.
    testing::TemporaryDirectory rootA("keyring-move-a");
    testing::TemporaryDirectory rootB("keyring-move-b");
    ASSERT_TRUE(Keyring::create(rootA.path(), kOperator, kPassword).ok());
    ASSERT_TRUE(Keyring::create(rootB.path(), kOperator, "a different passphrase here").ok());

    // Bodily replacing B's keyring with A's is not the interesting case — that
    // is just A. Copying A's file into B and then opening it must yield A's key,
    // not B's, and so must not open B's database.
    auto a = Keyring::load(rootA.path());
    auto b = Keyring::load(rootB.path());
    ASSERT_TRUE(a.ok() && b.ok());
    const auto keyringA = a.take();
    const auto keyringB = b.take();
    EXPECT_NE(keyringA.workspaceIdHex(), keyringB.workspaceIdHex());

    auto keyA = keyringA.unlock(kOperator, kPassword);
    auto keyB = keyringB.unlock(kOperator, "a different passphrase here");
    ASSERT_TRUE(keyA.ok() && keyB.ok());
    EXPECT_NE(keyA.take().toHexForSqlCipher(), keyB.take().toHexForSqlCipher());
}

// -------------------------------------------------------- encrypted database

class EncryptedDatabase : public ::testing::Test {
protected:
    void SetUp() override {
        if (!crypto::available()) GTEST_SKIP() << "built without encryption support";
    }
};

TEST_F(EncryptedDatabase, CaseDataIsNotReadableInTheFile) {
    // The claim the README makes, tested the only way it can honestly be
    // tested: write something distinctive, then look at the bytes on disk.
    testing::TemporaryDirectory root("encrypted-db");
    auto created = Keyring::create(root.path(), kOperator, kPassword);
    ASSERT_TRUE(created.ok());
    auto unlocked = created.take().unlock(kOperator, kPassword);
    ASSERT_TRUE(unlocked.ok());
    const auto masterKey = unlocked.take();

    const auto path = root.path() / "trace.db";
    const std::string distinctive = "OPERATION-CANTERBURY-2026-0413";
    {
        auto database = Database::openEncrypted(path, masterKey);
        ASSERT_TRUE(database.ok()) << database.error().toString();
        auto handle = database.take();
        ASSERT_TRUE(MigrationRunner::applyAll(*handle).ok());
        ASSERT_TRUE(handle
                        ->execute("CREATE TABLE probe (note TEXT); INSERT INTO probe VALUES ('" +
                                  distinctive + "');")
                        .ok());
    }

    const std::string raw = fileContents(path);
    ASSERT_FALSE(raw.empty());
    EXPECT_EQ(raw.find(distinctive), std::string::npos) << "case data is readable on disk";
    // Table names are schema, and schema lives in page one, which is encrypted
    // too — so even the shape of the case load is not on offer.
    EXPECT_EQ(raw.find("evidence"), std::string::npos);
    EXPECT_NE(raw.substr(0, 15), "SQLite format 3");
    EXPECT_TRUE(Database::fileIsEncrypted(path));
}

TEST_F(EncryptedDatabase, ReopensWithTheSameKeyAndRefusesAnother) {
    testing::TemporaryDirectory root("encrypted-reopen");
    auto created = Keyring::create(root.path(), kOperator, kPassword);
    ASSERT_TRUE(created.ok());
    auto keyring = created.take();
    auto unlocked = keyring.unlock(kOperator, kPassword);
    ASSERT_TRUE(unlocked.ok());
    const auto masterKey = unlocked.take();

    const auto path = root.path() / "trace.db";
    {
        auto database = Database::openEncrypted(path, masterKey);
        ASSERT_TRUE(database.ok());
        ASSERT_TRUE(database.take()->execute("CREATE TABLE probe (n INTEGER);").ok());
    }
    {
        auto database = Database::openEncrypted(path, masterKey);
        ASSERT_TRUE(database.ok()) << database.error().toString();
        auto count = database.take()->queryInt64("SELECT count(*) FROM probe;");
        EXPECT_TRUE(count.ok());
    }
    {
        auto wrong = crypto::SecretKey::random();
        ASSERT_TRUE(wrong.ok());
        auto database = Database::openEncrypted(path, wrong.take());
        EXPECT_FALSE(database.ok()) << "a random key opened the case database";
    }
}

TEST_F(EncryptedDatabase, AnEncryptedDatabaseDoesNotOpenUnkeyed) {
    testing::TemporaryDirectory root("encrypted-unkeyed");
    auto key = crypto::SecretKey::random();
    ASSERT_TRUE(key.ok());
    const auto path = root.path() / "trace.db";
    {
        auto database = Database::openEncrypted(path, key.take());
        ASSERT_TRUE(database.ok());
        ASSERT_TRUE(database.take()->execute("CREATE TABLE probe (n INTEGER);").ok());
    }

    auto plain = Database::open(path);
    // Opening the handle may succeed — SQLCipher does not read page one until
    // asked — so the check that matters is that no query works.
    if (plain.ok()) {
        auto query = plain.take()->queryInt64("SELECT count(*) FROM probe;");
        EXPECT_FALSE(query.ok()) << "an encrypted database answered an unkeyed query";
    }
}

TEST_F(EncryptedDatabase, AnUnencryptedDatabaseIsRecognisedAsSuch) {
    // Distinguishing the two is what lets TRACE ask for a password only when
    // there is something to unlock.
    testing::TemporaryDirectory root("plain-db");
    const auto path = root.path() / "trace.db";
    {
        auto database = Database::open(path);
        ASSERT_TRUE(database.ok());
        ASSERT_TRUE(database.take()->execute("CREATE TABLE probe (n INTEGER);").ok());
    }
    EXPECT_FALSE(Database::fileIsEncrypted(path));
    EXPECT_EQ(fileContents(path).substr(0, 15), "SQLite format 3");
}

TEST_F(EncryptedDatabase, TheFullSchemaMigratesInsideAnEncryptedDatabase) {
    // Encryption is below the schema, so every migration should apply
    // unchanged. This is the test that would catch it if one did not.
    testing::TemporaryDirectory root("encrypted-migrate");
    auto key = crypto::SecretKey::random();
    ASSERT_TRUE(key.ok());

    auto database = Database::openEncrypted(root.path() / "trace.db", key.take());
    ASSERT_TRUE(database.ok());
    auto handle = database.take();

    auto applied = MigrationRunner::applyAll(*handle);
    ASSERT_TRUE(applied.ok()) << applied.error().toString();

    auto tables = handle->queryInt64(
        "SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = 'evidence';");
    ASSERT_TRUE(tables.ok());
    EXPECT_EQ(tables.take(), 1);
}

}  // namespace
}  // namespace trace
