// Integrity Failure Test (Phase 0 acceptance §26).
//
// A deliberately altered *test copy* must be detected. Nothing here touches a
// real evidence file: the fixture is copied into a scratch directory first, and
// what gets modified is the managed copy inside that scratch case.
#include <gtest/gtest.h>
#include <iterator>

#include <fstream>

#include "core/security/sha256.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

class IntegrityFailureTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory = std::make_unique<testing::TemporaryDirectory>("trace-integrity");
        stack = std::make_unique<testing::TestStack>(testing::TestStack::create(directory->path()));

        source = directory->path() / "incoming" / "sample.mp4";
        std::filesystem::create_directories(source.parent_path());
        std::filesystem::copy_file(testing::sampleVideoPath(), source);

        CaseDraft draft;
        draft.title = "Integrity check";
        auto created = stack->cases->createCase(draft);
        ASSERT_TRUE(created.ok());
        owner = created.take();

        IngestRequest request;
        request.caseId = owner.id;
        request.sourcePath = source;
        auto imported = stack->evidence->ingest(request);
        ASSERT_TRUE(imported.ok()) << imported.error().toString();
        evidence = imported.value().evidence;
        managed = stack->layout->resolve(evidence.storageRelPath);
    }

    /// Flips one byte in the managed *test* copy.
    void tamperWithManagedCopy() {
        std::filesystem::permissions(managed, std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add);
        std::fstream file(managed, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file.seekg(1000, std::ios::beg);
        char byte = 0;
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0xFF);
        file.seekp(1000, std::ios::beg);
        file.write(&byte, 1);
        file.close();
    }

    std::unique_ptr<testing::TemporaryDirectory> directory;
    std::unique_ptr<testing::TestStack> stack;
    std::filesystem::path source;
    std::filesystem::path managed;
    Case owner;
    Evidence evidence;
};

TEST_F(IntegrityFailureTest, VerificationPassesBeforeTampering) {
    auto verified = stack->integrity->verify(evidence, owner.caseNumber);
    ASSERT_TRUE(verified.ok()) << verified.error().toString();
    EXPECT_TRUE(verified.value().verified);
    EXPECT_EQ(verified.value().storedSha256, verified.value().computedSha256);
    EXPECT_GT(verified.value().bytesRead, 0);
}

TEST_F(IntegrityFailureTest, ASingleAlteredByteIsDetected) {
    const std::string recordedDigest = evidence.sha256;
    tamperWithManagedCopy();

    auto verified = stack->integrity->verify(evidence, owner.caseNumber);
    ASSERT_TRUE(verified.ok()) << verified.error().toString();
    EXPECT_FALSE(verified.value().verified) << "a modified file was reported as verified";
    EXPECT_EQ(verified.value().storedSha256, recordedDigest);
    EXPECT_NE(verified.value().computedSha256, recordedDigest);

    // The stored baseline must never be replaced by the new digest.
    auto reloaded = stack->evidence->findById(evidence.id);
    ASSERT_TRUE(reloaded.ok());
    ASSERT_TRUE(reloaded.value().has_value());
    EXPECT_EQ(reloaded.value()->sha256, recordedDigest)
        << "the recorded digest was overwritten by a failed verification";
    EXPECT_EQ(reloaded.value()->integrityStatus, IntegrityStatus::Failed);
    EXPECT_TRUE(reloaded.value()->lastVerifiedAt.has_value());

    AuditQuery query;
    query.action = AuditAction::IntegrityFailed;
    auto events = stack->audit->list(query);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1u);
    const AuditEvent& event = events.value().front();
    EXPECT_EQ(event.outcome, "failure");
    EXPECT_NE(event.description.find("INTEGRITY FAILURE"), std::string::npos);
    EXPECT_NE(event.detailsJson.find(recordedDigest), std::string::npos);
    EXPECT_NE(event.detailsJson.find(verified.value().computedSha256), std::string::npos);
    EXPECT_NE(event.detailsJson.find("\"stored_digest_replaced\":false"), std::string::npos);
}

TEST_F(IntegrityFailureTest, TruncationIsDetected) {
    const std::string recordedDigest = evidence.sha256;
    std::filesystem::permissions(managed, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    std::filesystem::resize_file(managed, std::filesystem::file_size(managed) / 2);

    auto verified = stack->integrity->verify(evidence, owner.caseNumber);
    ASSERT_TRUE(verified.ok());
    EXPECT_FALSE(verified.value().verified);
    EXPECT_NE(verified.value().computedSha256, recordedDigest);
}

TEST_F(IntegrityFailureTest, MissingManagedFileIsReportedAsAnErrorNotAsCorruption) {
    std::filesystem::permissions(managed, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    std::filesystem::remove(managed);

    auto verified = stack->integrity->verify(evidence, owner.caseNumber);
    ASSERT_FALSE(verified.ok());
    EXPECT_EQ(verified.error().code(), ErrorCode::NotFound);

    auto reloaded = stack->evidence->findById(evidence.id);
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value()->integrityStatus, IntegrityStatus::Error);
    EXPECT_EQ(reloaded.value()->sha256, evidence.sha256);

    AuditQuery query;
    query.action = AuditAction::IntegrityFailed;
    auto events = stack->audit->list(query);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1u);
    EXPECT_NE(events.value().front().detailsJson.find("file_missing"), std::string::npos);
}

TEST_F(IntegrityFailureTest, TheOriginalSourceFileIsNeverTouchedByAnyOfThis) {
    const std::string fixtureDigest = Sha256::hash([&] {
        std::ifstream input(testing::sampleVideoPath(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }());

    tamperWithManagedCopy();
    stack->integrity->verify(evidence, owner.caseNumber);

    auto sourceDigest = hashFile(source);
    ASSERT_TRUE(sourceDigest.ok());
    EXPECT_EQ(sourceDigest.value(), fixtureDigest)
        << "the imported source file was altered by TRACE";
}

}  // namespace
}  // namespace trace
