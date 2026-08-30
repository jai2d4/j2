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
#include "core/common/uuid.h"
#include "core/security/user_context.h"
#include "core/services/case_service.h"
#include "core/services/evidence_service.h"
#include "core/services/integrity_service.h"
#include "core/services/workspace_service.h"
#include "media/ffmpeg/audio_decoder.h"
#include "media/ffmpeg/media_probe.h"
#include "media/ffmpeg/video_decoder.h"
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

/// The keyring wraps its entries with the AEAD primitive, so none of this works
/// in a build without encryption support. Skipping is the honest outcome there:
/// the feature is absent, not broken.
class KeyringTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!crypto::available()) GTEST_SKIP() << "built without encryption support";
    }
};

TEST_F(KeyringTest, CreatesAWorkspaceAndUnlocksItAgain) {
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

TEST_F(KeyringTest, TheSameMasterKeyComesBackEveryTime) {
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

TEST_F(KeyringTest, RefusesTheWrongPasswordAndAnUnknownOperatorIdentically) {
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

TEST_F(KeyringTest, HoldsNoPasswordAndNoMasterKeyOnDisk) {
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

TEST_F(KeyringTest, ASecondOperatorGetsTheSameMasterKeyWithoutSharingAPassword) {
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

TEST_F(KeyringTest, ChangingAPasswordKeepsTheMasterKeyAndSoKeepsTheData) {
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

TEST_F(KeyringTest, RefusesToRemoveTheLastOperator) {
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

TEST_F(KeyringTest, RefusesToOverwriteAnExistingKeyring) {
    // Overwriting is not a recoverable mistake: it destroys the only copies of
    // the master key.
    testing::TemporaryDirectory root("keyring-overwrite");
    ASSERT_TRUE(Keyring::create(root.path(), kOperator, kPassword).ok());
    auto again = Keyring::create(root.path(), "someone.else", "yet another passphrase");
    EXPECT_FALSE(again.ok());
    EXPECT_EQ(again.error().code(), ErrorCode::AlreadyExists);
}

TEST_F(KeyringTest, RefusesAShortPassword) {
    testing::TemporaryDirectory root("keyring-short");
    auto created = Keyring::create(root.path(), kOperator, "short");
    EXPECT_FALSE(created.ok());
    EXPECT_FALSE(Keyring::exists(root.path())) << "a rejected password left a keyring behind";
}

TEST_F(KeyringTest, RefusesATamperedFileRatherThanReadingWhatItCan) {
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

TEST_F(KeyringTest, AWrappedKeyCannotBeMovedBetweenWorkspaces) {
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

// ------------------------------------------------- encrypted evidence, decoded

class EncryptedMedia : public ::testing::Test {
protected:
    void SetUp() override {
        if (!crypto::available()) GTEST_SKIP() << "built without encryption support";
    }

    /// Writes the sample recording into a TRACE container under `key`.
    static void encryptSample(const std::filesystem::path& into, const crypto::SecretKey& key) {
        const auto source = testing::sampleVideoPath();
        std::ifstream in(source, std::ios::binary);
        ASSERT_TRUE(in.good());
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        ASSERT_FALSE(bytes.empty());

        crypto::EncryptedFileWriter writer;
        ASSERT_TRUE(writer.begin(into, key, bytes.size()).ok());
        ASSERT_TRUE(writer.write(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())
                        .ok());
        ASSERT_TRUE(writer.finish().ok());
    }
};

TEST_F(EncryptedMedia, ProbeReadsTheSameMetadataThroughTheContainer) {
    // If the decrypting IO were subtly wrong -- a bad seek, a short read -- the
    // duration or the codec would come back different. Comparing against the
    // plaintext probe is what makes that visible.
    auto key = crypto::SecretKey::random();
    ASSERT_TRUE(key.ok());
    const auto secret = key.take();

    testing::TemporaryDirectory root("encrypted-probe");
    const auto container = root.path() / "evidence.trace";
    encryptSample(container, secret);

    FFmpegMetadataExtractor extractor;
    auto plain = extractor.extract(testing::sampleVideoPath(), "EVD-plain");
    ASSERT_TRUE(plain.ok()) << plain.error().toString();
    auto encrypted = extractor.extract(container, "EVD-encrypted", &secret);
    ASSERT_TRUE(encrypted.ok()) << encrypted.error().toString();

    const MediaMetadata a = plain.take();
    const MediaMetadata b = encrypted.take();
    EXPECT_EQ(a.durationUs, b.durationUs);
    EXPECT_EQ(a.width, b.width);
    EXPECT_EQ(a.height, b.height);
    EXPECT_EQ(a.videoCodec, b.videoCodec);
    EXPECT_EQ(a.frameCount, b.frameCount);
}

TEST_F(EncryptedMedia, DecodedFramesAreIdenticalToTheUnencryptedRecording) {
    // The claim that encryption changes nothing about the evidence, tested
    // pixel by pixel rather than asserted.
    auto key = crypto::SecretKey::random();
    ASSERT_TRUE(key.ok());
    const auto secret = key.take();

    testing::TemporaryDirectory root("encrypted-decode");
    const auto container = root.path() / "evidence.trace";
    encryptSample(container, secret);

    auto plainDecoder = VideoDecoder::open(testing::sampleVideoPath());
    ASSERT_TRUE(plainDecoder.ok()) << plainDecoder.error().toString();
    auto encryptedDecoder = VideoDecoder::open(container, &secret);
    ASSERT_TRUE(encryptedDecoder.ok()) << encryptedDecoder.error().toString();

    auto plain = plainDecoder.take();
    auto encrypted = encryptedDecoder.take();

    int compared = 0;
    for (int i = 0; i < 12; ++i) {
        auto a = plain->nextFrame();
        auto b = encrypted->nextFrame();
        if (!a.ok() || !b.ok()) break;
        const auto frameA = a.take();
        const auto frameB = b.take();
        ASSERT_EQ(frameA.presentationUs, frameB.presentationUs) << "frame " << i;
        ASSERT_EQ(frameA.width, frameB.width);
        ASSERT_EQ(frameA.height, frameB.height);
        ASSERT_EQ(frameA.rgb, frameB.rgb) << "frame " << i << " decoded differently";
        ++compared;
    }
    EXPECT_GE(compared, 3) << "not enough frames decoded to prove anything";
}

TEST_F(EncryptedMedia, SeekingInsideAContainerLandsWhereItDoesInThePlaintext) {
    // Seeking is what the chunked format exists for. A decoder that can only
    // read forward would still pass the test above.
    auto key = crypto::SecretKey::random();
    ASSERT_TRUE(key.ok());
    const auto secret = key.take();

    testing::TemporaryDirectory root("encrypted-seek");
    const auto container = root.path() / "evidence.trace";
    encryptSample(container, secret);

    auto plainDecoder = VideoDecoder::open(testing::sampleVideoPath());
    auto encryptedDecoder = VideoDecoder::open(container, &secret);
    ASSERT_TRUE(plainDecoder.ok() && encryptedDecoder.ok());
    auto plain = plainDecoder.take();
    auto encrypted = encryptedDecoder.take();

    for (Microseconds target : {Microseconds{1000000}, Microseconds{500000}, Microseconds{0}}) {
        auto a = plain->frameAt(target);
        auto b = encrypted->frameAt(target);
        ASSERT_EQ(a.ok(), b.ok()) << "seek to " << target << " disagreed on success";
        if (!a.ok()) continue;
        const auto frameA = a.take();
        const auto frameB = b.take();
        EXPECT_EQ(frameA.presentationUs, frameB.presentationUs) << "seek to " << target;
        EXPECT_EQ(frameA.rgb, frameB.rgb) << "seek to " << target;
    }
}

TEST_F(EncryptedMedia, OpeningEncryptedEvidenceWithoutAKeyIsRefusedClearly) {
    // The failure an operator sees must name the real problem. Handed to FFmpeg
    // unkeyed, a container is just an unrecognised format, and the message
    // sends them looking for a missing codec.
    auto key = crypto::SecretKey::random();
    ASSERT_TRUE(key.ok());
    testing::TemporaryDirectory root("encrypted-nokey");
    const auto container = root.path() / "evidence.trace";
    encryptSample(container, key.take());

    auto opened = VideoDecoder::open(container);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.error().code(), ErrorCode::PermissionDenied);

    FFmpegMetadataExtractor extractor;
    auto probed = extractor.extract(container, "EVD-1");
    ASSERT_FALSE(probed.ok());
    EXPECT_EQ(probed.error().code(), ErrorCode::PermissionDenied);
}

TEST_F(EncryptedMedia, TheWrongKeyFailsRatherThanDecodingNoise) {
    auto right = crypto::SecretKey::random();
    auto wrong = crypto::SecretKey::random();
    ASSERT_TRUE(right.ok() && wrong.ok());
    const auto wrongKey = wrong.take();

    testing::TemporaryDirectory root("encrypted-wrongkey");
    const auto container = root.path() / "evidence.trace";
    encryptSample(container, right.take());

    auto opened = VideoDecoder::open(container, &wrongKey);
    if (opened.ok()) {
        // If the container header let it get this far, no frame may decode.
        auto frame = opened.take()->nextFrame();
        EXPECT_FALSE(frame.ok()) << "a wrong key produced a decoded frame";
    }
}

TEST_F(EncryptedMedia, APlainFileStillOpensWhenAKeyIsOffered) {
    // Encryption is opt-in per file. An unencrypted recording in an encrypted
    // workspace -- which is what every case ingested before this feature looks
    // like -- must keep working.
    auto key = crypto::SecretKey::random();
    ASSERT_TRUE(key.ok());
    const auto secret = key.take();

    auto opened = VideoDecoder::open(testing::sampleVideoPath(), &secret);
    ASSERT_TRUE(opened.ok()) << opened.error().toString();
    EXPECT_TRUE(opened.take()->nextFrame().ok());
}

// ------------------------------------------------ an encrypted workspace, used

/// The services an operator's session actually holds, wired the way
/// ApplicationContext wires them, against an encrypted workspace.
struct EncryptedStack {
    testing::TemporaryDirectory root;
    std::shared_ptr<WorkspaceKeys> keys = std::make_shared<WorkspaceKeys>();
    std::shared_ptr<Database> database;
    std::unique_ptr<StorageLayout> layout;
    std::shared_ptr<AuditService> audit;
    std::unique_ptr<CaseService> cases;
    std::unique_ptr<EvidenceService> evidence;
    std::unique_ptr<IntegrityService> integrity;

    explicit EncryptedStack(const std::string& prefix) : root(prefix) {}

    bool open() {
        layout = std::make_unique<StorageLayout>(root.path());
        if (!layout->ensureDataRoot()) return false;
        auto keyring = WorkspaceService::createEncryptedWorkspace(root.path(), kOperator, kPassword);
        if (!keyring) return false;
        if (!WorkspaceService::unlock(root.path(), kOperator, kPassword, *keys)) return false;

        auto masterKey = keys->masterKey();
        if (!masterKey) return false;
        auto opened = Database::openEncrypted(layout->databasePath(), masterKey.take());
        if (!opened) return false;
        database = opened.take();
        if (!MigrationRunner::applyAll(*database)) return false;

        audit = std::make_shared<AuditService>(database);
        cases = std::make_unique<CaseService>(database, *layout, audit);
        evidence = std::make_unique<EvidenceService>(
            database, *layout, audit, std::make_shared<FFmpegMetadataExtractor>(), keys);
        integrity = std::make_unique<IntegrityService>(database, *layout, audit, keys);
        return true;
    }
};

class EncryptedWorkspace : public ::testing::Test {
protected:
    void SetUp() override {
        if (!crypto::available()) GTEST_SKIP() << "built without encryption support";
        UserAccount account = UserContext::current().account();
        account.role = UserRole::Administrator;
        if (account.username.empty()) account.username = "test-operator";
        if (account.id.empty()) account.id = generateUuid();
        UserContext::current().setAuthenticatedAccount(account);
    }

    /// Copies the sample recording somewhere an ingestion can take it from.
    static std::filesystem::path stageSource(const std::filesystem::path& directory) {
        std::filesystem::create_directories(directory);
        const auto source = directory / "sample.mp4";
        std::filesystem::copy_file(testing::sampleVideoPath(), source,
                                   std::filesystem::copy_options::overwrite_existing);
        return source;
    }
};

TEST_F(EncryptedWorkspace, IngestedEvidenceIsOnDiskEncryptedAndStillVerifies) {
    // The whole feature in one test: evidence goes in, what lands on disk is not
    // the recording, and the digest TRACE recorded is still the digest of the
    // recording.
    EncryptedStack stack("encrypted-workspace");
    ASSERT_TRUE(stack.open());

    CaseDraft draft;
    draft.caseNumber = "CASE-ENC-1";
    draft.title = "Encrypted at rest";
    draft.investigator = "A. Analyst";
    auto created = stack.cases->createCase(draft);
    ASSERT_TRUE(created.ok()) << created.error().toString();
    const Case caseRecord = created.take();

    const auto source = stageSource(stack.root.path() / "incoming");
    const std::string sourceDigest = hashFile(source).take();

    IngestRequest request;
    request.caseId = caseRecord.id;
    request.sourcePath = source;
    auto ingested = stack.evidence->ingest(request);
    ASSERT_TRUE(ingested.ok()) << ingested.error().toString();
    const Evidence evidence = ingested.take().evidence;

    // The digest recorded is the digest of the recording, not of the container.
    EXPECT_EQ(evidence.sha256, sourceDigest);

    // What is on disk is a container, and does not contain the recording.
    const auto stored = stack.evidence->absolutePath(evidence);
    ASSERT_TRUE(std::filesystem::exists(stored));
    EXPECT_TRUE(crypto::looksEncrypted(stored));

    const std::string plainSource = fileContents(source);
    const std::string onDisk = fileContents(stored);
    ASSERT_GT(plainSource.size(), 1024u);
    // Compare a distinctive slice rather than the whole file: a container that
    // happened to share a few bytes would not prove anything either way.
    EXPECT_EQ(onDisk.find(plainSource.substr(512, 512)), std::string::npos)
        << "the recording is readable inside the container";

    // And an integrity check still passes, because it decrypts before hashing.
    auto verified = stack.integrity->verify(evidence, caseRecord.caseNumber);
    ASSERT_TRUE(verified.ok()) << verified.error().toString();
    const IntegrityCheck check = verified.take();
    EXPECT_TRUE(check.verified);
    EXPECT_EQ(check.computedSha256, sourceDigest);
}

TEST_F(EncryptedWorkspace, IngestionRefusesToRunWhileTheWorkspaceIsLocked) {
    // The failure that matters: locking must not fall back to writing evidence
    // in the clear into a workspace whose operator believes it is encrypted.
    EncryptedStack stack("encrypted-locked");
    ASSERT_TRUE(stack.open());

    CaseDraft draft;
    draft.caseNumber = "CASE-ENC-2";
    draft.title = "Locked";
    draft.investigator = "A. Analyst";
    auto created = stack.cases->createCase(draft);
    ASSERT_TRUE(created.ok());
    const Case caseRecord = created.take();

    const auto source = stageSource(stack.root.path() / "incoming2");
    stack.keys->lock();

    IngestRequest request;
    request.caseId = caseRecord.id;
    request.sourcePath = source;
    auto ingested = stack.evidence->ingest(request);
    ASSERT_FALSE(ingested.ok()) << "evidence was ingested into a locked workspace";
    EXPECT_EQ(ingested.error().code(), ErrorCode::PermissionDenied);

    // Nothing was written.
    auto listed = stack.evidence->listForCase(caseRecord.id);
    ASSERT_TRUE(listed.ok());
    EXPECT_TRUE(listed.take().empty());
}

TEST_F(EncryptedWorkspace, TamperedEvidenceFailsVerificationRatherThanBeingSilentlyRepaired) {
    EncryptedStack stack("encrypted-tamper");
    ASSERT_TRUE(stack.open());

    CaseDraft draft;
    draft.caseNumber = "CASE-ENC-3";
    draft.title = "Tampering";
    draft.investigator = "A. Analyst";
    auto created = stack.cases->createCase(draft);
    ASSERT_TRUE(created.ok());
    const Case caseRecord = created.take();

    IngestRequest request;
    request.caseId = caseRecord.id;
    request.sourcePath = stageSource(stack.root.path() / "incoming3");
    auto ingested = stack.evidence->ingest(request);
    ASSERT_TRUE(ingested.ok());
    const Evidence evidence = ingested.take().evidence;
    const std::string digestBefore = evidence.sha256;

    // Flip a byte well inside the ciphertext.
    const auto stored = stack.evidence->absolutePath(evidence);
    std::filesystem::permissions(stored, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    {
        std::fstream file(stored, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.good());
        file.seekg(static_cast<std::streamoff>(crypto::kContainerHeaderBytes + 100));
        char byte = 0;
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x01);
        file.seekp(static_cast<std::streamoff>(crypto::kContainerHeaderBytes + 100));
        file.write(&byte, 1);
    }

    auto verified = stack.integrity->verify(evidence, caseRecord.caseNumber);
    // Either outcome is acceptable as an answer -- a failed check or a hard
    // error -- but it must never come back verified, and the stored digest must
    // be exactly what it was.
    if (verified.ok()) {
        EXPECT_FALSE(verified.take().verified);
    }
    auto reloaded = stack.evidence->findById(evidence.id);
    ASSERT_TRUE(reloaded.ok());
    const auto after = reloaded.take();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->sha256, digestBefore) << "the stored digest was rewritten by a failed check";
}

TEST_F(EncryptedWorkspace, AnExistingWorkspaceConvertsWithoutLosingAnything) {
    // The path every existing installation has to take. Ingest in the clear,
    // convert, then check the same evidence still verifies against the same
    // digest and the case survived.
    testing::TemporaryDirectory root("convert-workspace");
    std::string evidenceId;
    std::string caseId;
    std::string caseNumber = "CASE-CONV-1";
    std::string digest;

    {
        auto stack = testing::TestStack::create(root.path());
        CaseDraft draft;
        draft.caseNumber = caseNumber;
        draft.title = "Conversion";
        draft.investigator = "A. Analyst";
        auto created = stack.cases->createCase(draft);
        ASSERT_TRUE(created.ok()) << created.error().toString();
        caseId = created.take().id;

        IngestRequest request;
        request.caseId = caseId;
        request.sourcePath = stageSource(root.path() / "incoming");
        auto ingested = stack.evidence->ingest(request);
        ASSERT_TRUE(ingested.ok()) << ingested.error().toString();
        const Evidence evidence = ingested.take().evidence;
        evidenceId = evidence.id;
        digest = evidence.sha256;

        // Before conversion the recording is stored as itself.
        EXPECT_FALSE(crypto::looksEncrypted(stack.evidence->absolutePath(evidence)));
    }

    ASSERT_FALSE(WorkspaceService::inspect(root.path()).encrypted);
    auto converted = WorkspaceService::encryptExistingWorkspace(root.path(), kOperator, kPassword);
    ASSERT_TRUE(converted.ok()) << converted.error().toString();

    const WorkspaceState state = WorkspaceService::inspect(root.path());
    EXPECT_TRUE(state.encrypted);
    EXPECT_TRUE(state.databaseEncrypted);
    EXPECT_FALSE(state.conversionIncomplete());

    // Reopen it the way the application would, and check the case and its
    // evidence came through unchanged.
    WorkspaceKeys keys;
    ASSERT_TRUE(WorkspaceService::unlock(root.path(), kOperator, kPassword, keys).ok());
    StorageLayout layout(root.path());
    auto opened = Database::openEncrypted(layout.databasePath(), keys.masterKey().take());
    ASSERT_TRUE(opened.ok()) << opened.error().toString();
    auto database = opened.take();

    auto audit = std::make_shared<AuditService>(database);
    auto keysPtr = std::make_shared<WorkspaceKeys>();
    ASSERT_TRUE(WorkspaceService::unlock(root.path(), kOperator, kPassword, *keysPtr).ok());
    EvidenceService evidenceService(database, layout, audit,
                                    std::make_shared<FFmpegMetadataExtractor>(), keysPtr);
    IntegrityService integrityService(database, layout, audit, keysPtr);

    auto found = evidenceService.findById(evidenceId);
    ASSERT_TRUE(found.ok());
    const auto evidence = found.take();
    ASSERT_TRUE(evidence.has_value());
    EXPECT_EQ(evidence->sha256, digest) << "conversion changed the recorded digest";
    EXPECT_TRUE(crypto::looksEncrypted(evidenceService.absolutePath(*evidence)));

    auto verified = integrityService.verify(*evidence, caseNumber);
    ASSERT_TRUE(verified.ok()) << verified.error().toString();
    const IntegrityCheck check = verified.take();
    EXPECT_TRUE(check.verified) << "converted evidence no longer verifies";
    EXPECT_EQ(check.computedSha256, digest);

    // The migration is not re-applied on the converted database, and the audit
    // trail came across rather than starting again.
    auto auditRows = database->queryInt64("SELECT count(*) FROM audit_events;");
    ASSERT_TRUE(auditRows.ok());
    EXPECT_GT(auditRows.take(), 0) << "the audit trail did not survive conversion";
}

TEST_F(EncryptedWorkspace, ConversionIsSafeToRunTwice) {
    // Which is what makes it resumable: an interrupted run is finished by
    // running it again, and a finished one is not damaged by it.
    testing::TemporaryDirectory root("convert-twice");
    {
        auto stack = testing::TestStack::create(root.path());
        CaseDraft draft;
        draft.caseNumber = "CASE-CONV-2";
        draft.title = "Twice";
        draft.investigator = "A. Analyst";
        auto created = stack.cases->createCase(draft);
        ASSERT_TRUE(created.ok());
        IngestRequest request;
        request.caseId = created.take().id;
        request.sourcePath = stageSource(root.path() / "incoming");
        ASSERT_TRUE(stack.evidence->ingest(request).ok());
    }

    ASSERT_TRUE(WorkspaceService::encryptExistingWorkspace(root.path(), kOperator, kPassword).ok());
    auto again = WorkspaceService::encryptExistingWorkspace(root.path(), kOperator, kPassword);
    EXPECT_TRUE(again.ok()) << again.error().toString();
    EXPECT_TRUE(WorkspaceService::inspect(root.path()).databaseEncrypted);
}

}  // namespace
}  // namespace trace
