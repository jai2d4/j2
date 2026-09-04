// Evidence Ingestion Test (Phase 0 acceptance §26).
//
// Imports the known sample file and checks every claim ingestion makes: the
// managed copy exists, is byte-identical, hashes to an independently computed
// digest, is recorded in the database with real metadata, and leaves an audit
// trail. The source file must come out untouched.
#include <gtest/gtest.h>

#include <fstream>

#include "core/security/sha256.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

class IngestionTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory = std::make_unique<testing::TemporaryDirectory>("trace-ingest");
        stack = std::make_unique<testing::TestStack>(testing::TestStack::create(directory->path()));

        // Work from a copy so the checked-in fixture is never at risk.
        source = directory->path() / "incoming" / "sample.mp4";
        std::filesystem::create_directories(source.parent_path());
        std::filesystem::copy_file(testing::sampleVideoPath(), source);

        CaseDraft draft;
        draft.title = "Sample Incident";
        draft.investigator = "A. Analyst";
        auto created = stack->cases->createCase(draft);
        ASSERT_TRUE(created.ok()) << created.error().toString();
        owner = created.take();
    }

    std::unique_ptr<testing::TemporaryDirectory> directory;
    std::unique_ptr<testing::TestStack> stack;
    std::filesystem::path source;
    Case owner;
};

TEST_F(IngestionTest, ImportsPreservesHashesAndRecords) {
    const std::string sourceBytes = readAll(source);
    const std::string expectedDigest = Sha256::hash(sourceBytes);
    const auto sourceSize = std::filesystem::file_size(source);

    std::vector<IngestStage> observedStages;
    IngestRequest request;
    request.caseId = owner.id;
    request.sourcePath = source;
    request.description = "Camera 3, front entrance";

    auto imported = stack->evidence->ingest(request, [&](const IngestProgress& progress) {
        if (observedStages.empty() || observedStages.back() != progress.stage) {
            observedStages.push_back(progress.stage);
        }
        EXPECT_GE(progress.overallFraction, 0.0);
        EXPECT_LE(progress.overallFraction, 1.0);
        return true;
    });
    ASSERT_TRUE(imported.ok()) << imported.error().toString();
    const IngestOutcome outcome = imported.take();
    const Evidence& evidence = outcome.evidence;

    // 1. the operator's file is untouched
    EXPECT_EQ(readAll(source), sourceBytes) << "the source file was modified";
    EXPECT_EQ(std::filesystem::file_size(source), sourceSize);

    // 2. the managed copy exists and is byte-identical
    const auto managed = stack->layout->resolve(evidence.storageRelPath);
    ASSERT_TRUE(std::filesystem::exists(managed)) << managed;
    EXPECT_EQ(std::filesystem::file_size(managed), sourceSize);
    EXPECT_EQ(readAll(managed), sourceBytes);
    EXPECT_NE(managed, source);
    EXPECT_TRUE(managed.string().find("originals") != std::string::npos);

    // 3. the digest matches one computed independently of the ingest pipeline
    EXPECT_EQ(evidence.sha256, expectedDigest);
    EXPECT_EQ(evidence.sha256.size(), 64u);
    EXPECT_EQ(evidence.integrityStatus, IntegrityStatus::Verified);

    // 4. the record is complete
    EXPECT_EQ(evidence.evidenceNumber, "EVD-000001");
    EXPECT_EQ(evidence.originalFilename, "sample.mp4");
    EXPECT_EQ(evidence.caseId, owner.id);
    EXPECT_EQ(evidence.fileSize, static_cast<std::int64_t>(sourceSize));
    EXPECT_EQ(evidence.mediaType, MediaType::Video);
    EXPECT_EQ(evidence.mediaStatus, MediaStatus::Decodable);
    EXPECT_EQ(evidence.sourcePath, std::filesystem::absolute(source).string());
    EXPECT_FALSE(evidence.ingestedAt.empty());
    EXPECT_TRUE(evidence.sourceModifiedAt.has_value());

    auto reloaded = stack->evidence->findById(evidence.id);
    ASSERT_TRUE(reloaded.ok());
    ASSERT_TRUE(reloaded.value().has_value());
    EXPECT_EQ(reloaded.value()->sha256, expectedDigest);

    // 5. real metadata was extracted from the file
    ASSERT_TRUE(outcome.metadataExtracted) << outcome.metadataError;
    auto metadata = stack->evidence->metadataFor(evidence.id);
    ASSERT_TRUE(metadata.ok());
    ASSERT_TRUE(metadata.value().has_value());
    const MediaMetadata& media = *metadata.value();
    EXPECT_EQ(media.videoCodec.value_or(""), "h264");
    EXPECT_EQ(media.width.value_or(0), 320);
    EXPECT_EQ(media.height.value_or(0), 240);
    EXPECT_NEAR(static_cast<double>(media.durationUs.value_or(0)), 8'000'000.0, 200'000.0);
    EXPECT_NEAR(media.averageFrameRate.value_or(0.0), 25.0, 0.01);
    EXPECT_EQ(media.videoStreamCount, 1);
    EXPECT_EQ(media.audioStreamCount, 1);
    EXPECT_EQ(media.audioCodec.value_or(""), "aac");
    EXPECT_EQ(media.streams.size(), 2u);
    EXPECT_EQ(media.extractionStatus, MetadataExtractionStatus::Ok);
    EXPECT_NE(media.rawMetadataJson.find("format_name"), std::string::npos);
    EXPECT_FALSE(media.extractor.empty());

    // 6. progress reported the real stages, in order, ending complete
    ASSERT_FALSE(observedStages.empty());
    EXPECT_EQ(observedStages.front(), IngestStage::Validating);
    EXPECT_EQ(observedStages.back(), IngestStage::Complete);
    EXPECT_NE(std::find(observedStages.begin(), observedStages.end(), IngestStage::CopyingOriginal),
              observedStages.end());
    EXPECT_NE(std::find(observedStages.begin(), observedStages.end(), IngestStage::VerifyingCopy),
              observedStages.end());

    // 7. the audit trail describes what happened
    AuditQuery query;
    query.caseId = owner.id;
    query.newestFirst = false;
    auto events = stack->audit->list(query);
    ASSERT_TRUE(events.ok());

    const auto has = [&](AuditAction action) {
        return std::any_of(events.value().begin(), events.value().end(),
                           [&](const AuditEvent& event) { return event.action == action; });
    };
    EXPECT_TRUE(has(AuditAction::CaseCreated));
    EXPECT_TRUE(has(AuditAction::EvidenceImportStarted));
    EXPECT_TRUE(has(AuditAction::EvidenceHashed));
    EXPECT_TRUE(has(AuditAction::EvidenceImported));
    EXPECT_TRUE(has(AuditAction::EvidenceMetadataExtracted));

    for (const auto& event : events.value()) {
        if (event.action != AuditAction::EvidenceImported) continue;
        EXPECT_EQ(event.evidenceNumber.value_or(""), "EVD-000001");
        EXPECT_NE(event.detailsJson.find(expectedDigest), std::string::npos)
            << "the digest belongs in the audit record";
        EXPECT_NE(event.detailsJson.find("sample.mp4"), std::string::npos);
    }
}

TEST_F(IngestionTest, SecondImportGetsItsOwnNumberAndStorageName) {
    IngestRequest request;
    request.caseId = owner.id;
    request.sourcePath = source;

    auto first = stack->evidence->ingest(request);
    ASSERT_TRUE(first.ok()) << first.error().toString();
    auto second = stack->evidence->ingest(request);
    ASSERT_TRUE(second.ok()) << second.error().toString();

    EXPECT_EQ(first.value().evidence.evidenceNumber, "EVD-000001");
    EXPECT_EQ(second.value().evidence.evidenceNumber, "EVD-000002");
    EXPECT_NE(first.value().evidence.storedFilename, second.value().evidence.storedFilename)
        << "managed names must not collide";
    EXPECT_EQ(first.value().evidence.sha256, second.value().evidence.sha256);
    EXPECT_TRUE(second.value().duplicateOfExisting)
        << "identical content in the same case should be flagged";
    EXPECT_EQ(second.value().duplicateEvidenceNumber, "EVD-000001");

    EXPECT_TRUE(std::filesystem::exists(
        stack->layout->resolve(first.value().evidence.storageRelPath)));
    EXPECT_TRUE(std::filesystem::exists(
        stack->layout->resolve(second.value().evidence.storageRelPath)));
}

TEST_F(IngestionTest, RejectingDuplicatesLeavesNothingBehind) {
    IngestRequest request;
    request.caseId = owner.id;
    request.sourcePath = source;
    ASSERT_TRUE(stack->evidence->ingest(request).ok());

    request.allowDuplicate = false;
    auto rejected = stack->evidence->ingest(request);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code(), ErrorCode::AlreadyExists);

    auto listed = stack->evidence->listForCase(owner.id);
    ASSERT_TRUE(listed.ok());
    EXPECT_EQ(listed.value().size(), 1u) << "the rejected import must not create a record";

    int filesInStorage = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(stack->layout->originalsDirectory(owner.id))) {
        (void)entry;
        ++filesInStorage;
    }
    EXPECT_EQ(filesInStorage, 1) << "the rejected import must not leave a file behind";
}

TEST_F(IngestionTest, MissingSourceFailsSafelyAndIsAudited) {
    IngestRequest request;
    request.caseId = owner.id;
    request.sourcePath = directory->path() / "not-there.mp4";

    auto failed = stack->evidence->ingest(request);
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.error().code(), ErrorCode::NotFound);

    auto listed = stack->evidence->listForCase(owner.id);
    ASSERT_TRUE(listed.ok());
    EXPECT_TRUE(listed.value().empty()) << "a failed import must not create an evidence record";

    AuditQuery query;
    query.action = AuditAction::EvidenceImportFailed;
    auto events = stack->audit->list(query);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1u);
    EXPECT_EQ(events.value().front().outcome, "failure");
    EXPECT_NE(events.value().front().detailsJson.find("\"source_modified\":false"),
              std::string::npos);
}

TEST_F(IngestionTest, NonMediaFileIsStoredHashedAndMarkedUndecodable) {
    // Integrity and decodability are different things: a text file is intact
    // evidence that simply is not media.
    const auto document = directory->path() / "incoming" / "statement.txt";
    { std::ofstream(document) << "Witness statement, page 1.\n"; }

    IngestRequest request;
    request.caseId = owner.id;
    request.sourcePath = document;
    auto imported = stack->evidence->ingest(request);
    ASSERT_TRUE(imported.ok()) << imported.error().toString();

    const Evidence& evidence = imported.value().evidence;
    EXPECT_EQ(evidence.integrityStatus, IntegrityStatus::Verified)
        << "the bytes are intact regardless of whether they decode";
    EXPECT_NE(evidence.mediaStatus, MediaStatus::Decodable);
    EXPECT_EQ(evidence.mediaType, MediaType::Document);
    EXPECT_FALSE(imported.value().metadataExtracted);
    EXPECT_EQ(evidence.sha256.size(), 64u);

    auto metadata = stack->evidence->metadataFor(evidence.id);
    ASSERT_TRUE(metadata.ok());
    ASSERT_TRUE(metadata.value().has_value());
    EXPECT_EQ(metadata.value()->extractionStatus, MetadataExtractionStatus::Failed);
    EXPECT_TRUE(metadata.value()->extractionError.has_value());
}

TEST_F(IngestionTest, CancellationRollsBackCompletely) {
    IngestRequest request;
    request.caseId = owner.id;
    request.sourcePath = source;

    auto cancelled = stack->evidence->ingest(request, [](const IngestProgress& progress) {
        return progress.stage != IngestStage::CopyingOriginal;
    });
    ASSERT_FALSE(cancelled.ok());
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);

    auto listed = stack->evidence->listForCase(owner.id);
    ASSERT_TRUE(listed.ok());
    EXPECT_TRUE(listed.value().empty());
    EXPECT_TRUE(std::filesystem::exists(source)) << "the source must survive a cancelled import";

    int filesInStorage = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(stack->layout->originalsDirectory(owner.id))) {
        (void)entry;
        ++filesInStorage;
    }
    EXPECT_EQ(filesInStorage, 0) << "a cancelled import must not leave a partial file";
}

}  // namespace
}  // namespace trace
