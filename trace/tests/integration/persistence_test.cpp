// Persistence Test (Phase 0 acceptance §26): create, import, bookmark, close
// everything, reopen, and confirm the case is exactly as it was left.
#include <algorithm>
#include <gtest/gtest.h>

#include "core/security/sha256.h"
#include "core/database/migrations.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

TEST(PersistenceTest, CaseEvidenceHashAndBookmarksSurviveARestart) {
    testing::TemporaryDirectory directory("trace-persist");
    const auto source = directory.path() / "incoming" / "sample.mp4";
    std::filesystem::create_directories(source.parent_path());
    std::filesystem::copy_file(testing::sampleVideoPath(), source);

    std::string caseId;
    std::string evidenceId;
    std::string digest;
    std::string storageRelPath;
    std::int64_t fileSize = 0;

    // ---------------------------------------------------------- first session
    {
        auto stack = testing::TestStack::create(directory.path());

        CaseDraft draft;
        draft.caseNumber = "CASE-0001";
        draft.title = "Sample Incident";
        draft.investigator = "A. Analyst";
        auto created = stack.cases->createCase(draft);
        ASSERT_TRUE(created.ok()) << created.error().toString();
        caseId = created.value().id;

        IngestRequest request;
        request.caseId = caseId;
        request.sourcePath = source;
        auto imported = stack.evidence->ingest(request);
        ASSERT_TRUE(imported.ok()) << imported.error().toString();
        evidenceId = imported.value().evidence.id;
        digest = imported.value().evidence.sha256;
        storageRelPath = imported.value().evidence.storageRelPath;
        fileSize = imported.value().evidence.fileSize;

        BookmarkDraft bookmark;
        bookmark.caseId = caseId;
        bookmark.evidenceId = evidenceId;
        bookmark.timestampUs = 5'000'000;
        bookmark.title = "Test Bookmark";
        bookmark.note = "Created during the persistence test";
        ASSERT_TRUE(stack.annotations->createBookmark(bookmark, "CASE-0001", "EVD-000001").ok());

        AnnotationDraft note;
        note.caseId = caseId;
        note.evidenceId = evidenceId;
        note.startUs = 2'000'000;
        note.endUs = 4'000'000;
        note.text = "Subject crosses the frame";
        ASSERT_TRUE(stack.annotations->createAnnotation(note, "CASE-0001", "EVD-000001").ok());
        // The stack (and its database connection) goes out of scope here — the
        // equivalent of closing TRACE.
    }

    // --------------------------------------------------------- second session
    {
        auto stack = testing::TestStack::create(directory.path());

        auto reopened = stack.cases->findByCaseNumber("CASE-0001");
        ASSERT_TRUE(reopened.ok());
        ASSERT_TRUE(reopened.value().has_value()) << "the case did not survive the restart";
        EXPECT_EQ(reopened.value()->id, caseId);
        EXPECT_EQ(reopened.value()->title, "Sample Incident");
        EXPECT_EQ(reopened.value()->investigator, "A. Analyst");

        auto evidence = stack.evidence->listForCase(caseId);
        ASSERT_TRUE(evidence.ok());
        ASSERT_EQ(evidence.value().size(), 1u);
        const Evidence& stored = evidence.value().front();
        EXPECT_EQ(stored.id, evidenceId);
        EXPECT_EQ(stored.evidenceNumber, "EVD-000001");
        EXPECT_EQ(stored.sha256, digest) << "the recorded digest changed across a restart";
        EXPECT_EQ(stored.fileSize, fileSize);
        EXPECT_EQ(stored.storageRelPath, storageRelPath);

        // The managed original is still on disk and still matches.
        const auto managed = stack.layout->resolve(stored.storageRelPath);
        ASSERT_TRUE(std::filesystem::exists(managed));
        auto rehashed = hashFile(managed);
        ASSERT_TRUE(rehashed.ok());
        EXPECT_EQ(rehashed.value(), digest);

        auto metadata = stack.evidence->metadataFor(evidenceId);
        ASSERT_TRUE(metadata.ok());
        ASSERT_TRUE(metadata.value().has_value());
        EXPECT_EQ(metadata.value()->videoCodec.value_or(""), "h264");
        EXPECT_EQ(metadata.value()->streams.size(), 2u);

        auto bookmarks = stack.annotations->bookmarksForEvidence(evidenceId);
        ASSERT_TRUE(bookmarks.ok());
        ASSERT_EQ(bookmarks.value().size(), 1u);
        EXPECT_EQ(bookmarks.value().front().timestampUs, 5'000'000);
        EXPECT_EQ(bookmarks.value().front().title, "Test Bookmark");
        EXPECT_EQ(bookmarks.value().front().note, "Created during the persistence test");

        auto annotations = stack.annotations->annotationsForEvidence(evidenceId);
        ASSERT_TRUE(annotations.ok());
        ASSERT_EQ(annotations.value().size(), 1u);
        EXPECT_EQ(annotations.value().front().endUs.value_or(0), 4'000'000);
        EXPECT_EQ(annotations.value().front().type, AnnotationType::RangeNote);

        // Verification after the restart still passes and is recorded.
        auto verified = stack.integrity->verify(stored, "CASE-0001");
        ASSERT_TRUE(verified.ok()) << verified.error().toString();
        EXPECT_TRUE(verified.value().verified);
        EXPECT_EQ(verified.value().computedSha256, digest);

        AuditQuery query;
        query.caseId = caseId;
        query.newestFirst = false;
        auto events = stack.audit->list(query);
        ASSERT_TRUE(events.ok());
        const auto has = [&](AuditAction action) {
            return std::any_of(events.value().begin(), events.value().end(),
                               [&](const AuditEvent& event) { return event.action == action; });
        };
        EXPECT_TRUE(has(AuditAction::CaseCreated));
        EXPECT_TRUE(has(AuditAction::EvidenceImported));
        EXPECT_TRUE(has(AuditAction::BookmarkCreated));
        EXPECT_TRUE(has(AuditAction::AnnotationCreated));
        EXPECT_TRUE(has(AuditAction::IntegrityVerified));

        // Sequence numbers keep increasing across sessions.
        std::int64_t previous = 0;
        for (const auto& event : events.value()) {
            EXPECT_GT(event.sequence, previous);
            previous = event.sequence;
        }
    }
}

TEST(PersistenceTest, SchemaVersionIsStableAcrossRestarts) {
    testing::TemporaryDirectory directory("trace-persist-schema");
    int firstVersion = 0;
    {
        auto stack = testing::TestStack::create(directory.path(), false);
        auto version = MigrationRunner::currentVersion(*stack.database);
        ASSERT_TRUE(version.ok());
        firstVersion = version.value();
        EXPECT_GE(firstVersion, 2);
    }
    {
        auto stack = testing::TestStack::create(directory.path(), false);
        auto version = MigrationRunner::currentVersion(*stack.database);
        ASSERT_TRUE(version.ok());
        EXPECT_EQ(version.value(), firstVersion);
    }
}

}  // namespace
}  // namespace trace
