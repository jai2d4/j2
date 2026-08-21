// Persistence round-trips for every entity, plus the numbering rules.
#include <gtest/gtest.h>

#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/database/migrations.h"
#include "core/repositories/annotation_repository.h"
#include "core/repositories/audit_repository.h"
#include "core/repositories/bookmark_repository.h"
#include "core/repositories/case_repository.h"
#include "core/repositories/evidence_repository.h"
#include "core/repositories/metadata_repository.h"
#include "core/repositories/provenance_repository.h"
#include "core/repositories/settings_repository.h"
#include "core/repositories/user_repository.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

class RepositoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = Database::openInMemory();
        ASSERT_TRUE(opened.ok()) << opened.error().toString();
        database = opened.take();
        ASSERT_TRUE(MigrationRunner::applyAll(*database).ok());
    }

    Case insertCase(const std::string& number = "CASE-0001") {
        CaseRepository repository(database);
        Case value;
        value.id = generateUuid();
        value.caseNumber = number;
        value.title = "Sample Incident";
        value.description = "A vehicle collision at a junction";
        value.status = CaseStatus::Open;
        value.investigator = "A. Analyst";
        value.agency = "Example Agency";
        value.location = "Junction of 5th and Main";
        value.notes = "Initial intake";
        value.createdAt = nowIso8601Utc();
        value.modifiedAt = value.createdAt;
        value.createdBy = "tester";
        value.storageRelPath = "cases/" + value.id;
        EXPECT_TRUE(repository.insert(value).ok());
        return value;
    }

    Evidence insertEvidence(const Case& owner, const std::string& number = "EVD-000001") {
        EvidenceRepository repository(database);
        Evidence value;
        value.id = generateUuid();
        value.caseId = owner.id;
        value.evidenceNumber = number;
        value.originalFilename = "sample.mp4";
        value.storedFilename = uuidToCompact(value.id) + "_sample.mp4";
        value.sourcePath = "/media/card/sample.mp4";
        value.storageRelPath = owner.storageRelPath + "/originals/" + value.storedFilename;
        value.mediaType = MediaType::Video;
        value.mimeType = "video/mp4";
        value.fileSize = 163644;
        value.sha256 = "b6fce02ac6639c23799e378df8ef3749314b9ec90c813da5cea8e426e795a21a";
        value.ingestedAt = nowIso8601Utc();
        value.modifiedAt = value.ingestedAt;
        value.ingestedBy = "tester";
        value.integrityStatus = IntegrityStatus::Verified;
        value.mediaStatus = MediaStatus::Decodable;
        EXPECT_TRUE(repository.insert(value).ok());
        return value;
    }

    std::shared_ptr<Database> database;
};

TEST_F(RepositoryTest, CaseRoundTripsEveryField) {
    const Case original = insertCase();
    CaseRepository repository(database);

    auto found = repository.findById(original.id);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    const Case& stored = *found.value();

    EXPECT_EQ(stored.caseNumber, original.caseNumber);
    EXPECT_EQ(stored.title, original.title);
    EXPECT_EQ(stored.description, original.description);
    EXPECT_EQ(stored.status, CaseStatus::Open);
    EXPECT_EQ(stored.investigator, original.investigator);
    EXPECT_EQ(stored.agency, original.agency);
    EXPECT_EQ(stored.location, original.location);
    EXPECT_EQ(stored.notes, original.notes);
    EXPECT_EQ(stored.storageRelPath, original.storageRelPath);

    auto byNumber = repository.findByCaseNumber("CASE-0001");
    ASSERT_TRUE(byNumber.ok());
    ASSERT_TRUE(byNumber.value().has_value());
    EXPECT_EQ(byNumber.value()->id, original.id);
}

TEST_F(RepositoryTest, CaseNumbersAreUniqueAndSequential) {
    CaseRepository repository(database);
    auto next = repository.nextCaseNumber();
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value(), "CASE-0001");

    insertCase("CASE-0001");
    next = repository.nextCaseNumber();
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value(), "CASE-0002");

    // An agency-supplied reference does not disturb the generated sequence.
    insertCase("OPS-2026-114");
    next = repository.nextCaseNumber();
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value(), "CASE-0002");

    Case duplicate = insertCase("CASE-0003");
    duplicate.id = generateUuid();
    EXPECT_FALSE(repository.insert(duplicate).ok()) << "case numbers must be unique";
}

TEST_F(RepositoryTest, CaseSearchAndFilter) {
    insertCase("CASE-0001");
    Case second = insertCase("CASE-0002");
    second.title = "Warehouse burglary";
    second.status = CaseStatus::Closed;
    CaseRepository repository(database);
    ASSERT_TRUE(repository.update(second).ok());

    CaseQuery byText;
    byText.text = "warehouse";
    auto results = repository.list(byText);
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results.value().size(), 1u);
    EXPECT_EQ(results.value().front().caseNumber, "CASE-0002");

    CaseQuery byStatus;
    byStatus.status = CaseStatus::Open;
    results = repository.list(byStatus);
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results.value().size(), 1u);
    EXPECT_EQ(results.value().front().caseNumber, "CASE-0001");
}

TEST_F(RepositoryTest, EvidenceRoundTripsAndNumbersPerCase) {
    const Case owner = insertCase();
    const Evidence stored = insertEvidence(owner);
    EvidenceRepository repository(database);

    auto found = repository.findById(stored.id);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    EXPECT_EQ(found.value()->sha256, stored.sha256);
    EXPECT_EQ(found.value()->fileSize, stored.fileSize);
    EXPECT_EQ(found.value()->mediaType, MediaType::Video);
    EXPECT_EQ(found.value()->integrityStatus, IntegrityStatus::Verified);
    EXPECT_EQ(found.value()->mimeType.value_or(""), "video/mp4");

    auto next = repository.nextEvidenceNumber(owner.id);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value(), "EVD-000002");

    // Numbering restarts inside a different case.
    const Case other = insertCase("CASE-0002");
    next = repository.nextEvidenceNumber(other.id);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value(), "EVD-000001");

    auto duplicates = repository.findByHash(stored.sha256, owner.id);
    ASSERT_TRUE(duplicates.ok());
    EXPECT_EQ(duplicates.value().size(), 1u);
}

TEST_F(RepositoryTest, IntegrityStatusUpdatesNeverTouchTheStoredDigest) {
    const Case owner = insertCase();
    const Evidence stored = insertEvidence(owner);
    EvidenceRepository repository(database);

    ASSERT_TRUE(repository.updateIntegrityStatus(stored.id, IntegrityStatus::Failed,
                                                 nowIso8601Utc())
                    .ok());
    auto found = repository.findById(stored.id);
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value()->integrityStatus, IntegrityStatus::Failed);
    EXPECT_EQ(found.value()->sha256, stored.sha256)
        << "a failed verification must never rewrite the recorded digest";
    EXPECT_TRUE(found.value()->lastVerifiedAt.has_value());
}

TEST_F(RepositoryTest, MetadataAndStreamsRoundTripWithNullsPreserved) {
    const Case owner = insertCase();
    const Evidence stored = insertEvidence(owner);

    MediaMetadata metadata;
    metadata.evidenceId = stored.id;
    metadata.containerFormat = "mov,mp4,m4a,3gp,3g2,mj2";
    metadata.durationUs = 8'000'000;
    metadata.videoCodec = "h264";
    metadata.width = 320;
    metadata.height = 240;
    metadata.averageFrameRate = 25.0;
    metadata.nominalFrameRate = 25.0;
    metadata.frameRateMode = FrameRateMode::ConstantSuspected;
    metadata.videoStreamCount = 1;
    metadata.audioStreamCount = 1;
    metadata.audioCodec = "aac";
    metadata.audioChannels = 1;
    metadata.rawMetadataJson = R"({"format":{"format_name":"mp4"}})";
    metadata.extractionStatus = MetadataExtractionStatus::Ok;
    metadata.extractor = "TRACE probe (test)";
    metadata.extractedAt = nowIso8601Utc();
    // Deliberately absent: GPS, make/model, bitrate.

    MediaStreamInfo video;
    video.id = generateUuid();
    video.evidenceId = stored.id;
    video.index = 0;
    video.type = StreamType::Video;
    video.codecName = "h264";
    video.width = 320;
    video.height = 240;
    video.frameRate = 25.0;
    metadata.streams.push_back(video);

    MediaStreamInfo audio;
    audio.id = generateUuid();
    audio.evidenceId = stored.id;
    audio.index = 1;
    audio.type = StreamType::Audio;
    audio.codecName = "aac";
    audio.sampleRate = 44100;
    audio.channels = 1;
    metadata.streams.push_back(audio);

    MetadataRepository repository(database);
    ASSERT_TRUE(repository.save(metadata).ok());

    auto found = repository.findByEvidenceId(stored.id);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    const MediaMetadata& reloaded = *found.value();
    EXPECT_EQ(reloaded.durationUs.value_or(0), 8'000'000);
    EXPECT_EQ(reloaded.videoCodec.value_or(""), "h264");
    EXPECT_EQ(reloaded.frameRateMode, FrameRateMode::ConstantSuspected);
    EXPECT_FALSE(reloaded.gpsLocation.has_value()) << "absent metadata must stay absent";
    EXPECT_FALSE(reloaded.make.has_value());
    EXPECT_EQ(reloaded.streams.size(), 2u);
    EXPECT_EQ(reloaded.streams[1].sampleRate.value_or(0), 44100);

    // Saving again replaces rather than duplicates the stream rows.
    ASSERT_TRUE(repository.save(metadata).ok());
    auto streams = repository.listStreams(stored.id);
    ASSERT_TRUE(streams.ok());
    EXPECT_EQ(streams.value().size(), 2u);
}

TEST_F(RepositoryTest, BookmarksAndAnnotationsPersistAndSortByPosition) {
    const Case owner = insertCase();
    const Evidence stored = insertEvidence(owner);

    BookmarkRepository bookmarks(database);
    for (const std::int64_t timestamp : {97'433'000, 5'000'000, 42'000'000}) {
        Bookmark bookmark;
        bookmark.id = generateUuid();
        bookmark.caseId = owner.id;
        bookmark.evidenceId = stored.id;
        bookmark.timestampUs = timestamp;
        bookmark.title = "Marker at " + formatTimecode(timestamp);
        bookmark.note = "note";
        bookmark.createdBy = "tester";
        bookmark.createdAt = nowIso8601Utc();
        bookmark.modifiedAt = bookmark.createdAt;
        ASSERT_TRUE(bookmarks.insert(bookmark).ok());
    }

    auto listed = bookmarks.listForEvidence(stored.id);
    ASSERT_TRUE(listed.ok());
    ASSERT_EQ(listed.value().size(), 3u);
    EXPECT_EQ(listed.value()[0].timestampUs, 5'000'000);
    EXPECT_EQ(listed.value()[2].timestampUs, 97'433'000);

    Bookmark edited = listed.value()[0];
    edited.title = "Person enters doorway";
    edited.modifiedAt = nowIso8601Utc();
    ASSERT_TRUE(bookmarks.update(edited).ok());
    auto reloaded = bookmarks.findById(edited.id);
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.value()->title, "Person enters doorway");

    AnnotationRepository annotations(database);
    Annotation point;
    point.id = generateUuid();
    point.caseId = owner.id;
    point.evidenceId = stored.id;
    point.type = AnnotationType::TimestampNote;
    point.startUs = 1'500'000;
    point.text = "Camera flare";
    point.createdBy = "tester";
    point.createdAt = nowIso8601Utc();
    point.modifiedAt = point.createdAt;
    ASSERT_TRUE(annotations.insert(point).ok());

    Annotation range = point;
    range.id = generateUuid();
    range.type = AnnotationType::RangeNote;
    range.startUs = 2'000'000;
    range.endUs = 4'000'000;
    range.text = "Subject crosses frame";
    ASSERT_TRUE(annotations.insert(range).ok());

    auto storedAnnotations = annotations.listForEvidence(stored.id);
    ASSERT_TRUE(storedAnnotations.ok());
    ASSERT_EQ(storedAnnotations.value().size(), 2u);
    EXPECT_FALSE(storedAnnotations.value()[0].endUs.has_value());
    EXPECT_EQ(storedAnnotations.value()[1].endUs.value_or(0), 4'000'000);
    EXPECT_EQ(storedAnnotations.value()[1].type, AnnotationType::RangeNote);
}

TEST_F(RepositoryTest, DeletingACaseCascadesToItsContentButNotToTheAuditTrail) {
    const Case owner = insertCase();
    const Evidence stored = insertEvidence(owner);

    BookmarkRepository bookmarks(database);
    Bookmark bookmark;
    bookmark.id = generateUuid();
    bookmark.caseId = owner.id;
    bookmark.evidenceId = stored.id;
    bookmark.timestampUs = 1'000'000;
    bookmark.title = "Marker";
    bookmark.createdAt = nowIso8601Utc();
    bookmark.modifiedAt = bookmark.createdAt;
    ASSERT_TRUE(bookmarks.insert(bookmark).ok());

    AuditRepository audit(database);
    AuditEvent event;
    event.id = generateUuid();
    event.occurredAt = nowIso8601Utc();
    event.action = AuditAction::EvidenceImported;
    event.actor = "tester";
    event.caseId = owner.id;
    event.caseNumber = owner.caseNumber;
    event.evidenceId = stored.id;
    event.evidenceNumber = stored.evidenceNumber;
    event.description = "Imported";
    ASSERT_TRUE(audit.append(event).ok());

    CaseRepository cases(database);
    ASSERT_TRUE(cases.remove(owner.id).ok());

    EvidenceRepository evidence(database);
    auto remaining = evidence.listForCase(owner.id);
    ASSERT_TRUE(remaining.ok());
    EXPECT_TRUE(remaining.value().empty());
    auto remainingBookmarks = bookmarks.listForEvidence(stored.id);
    ASSERT_TRUE(remainingBookmarks.ok());
    EXPECT_TRUE(remainingBookmarks.value().empty());

    auto history = audit.list({});
    ASSERT_TRUE(history.ok());
    ASSERT_EQ(history.value().size(), 1u);
    EXPECT_EQ(history.value().front().caseNumber.value_or(""), owner.caseNumber)
        << "the audit trail must survive deletion of the case it describes";
}

TEST_F(RepositoryTest, AuditSequenceIsMonotonicAndFilterable) {
    const Case owner = insertCase();
    AuditRepository repository(database);

    for (int i = 0; i < 5; ++i) {
        AuditEvent event;
        event.id = generateUuid();
        event.occurredAt = nowIso8601Utc();
        event.action = i % 2 == 0 ? AuditAction::EvidenceViewed : AuditAction::BookmarkCreated;
        event.actor = "tester";
        event.caseId = owner.id;
        event.caseNumber = owner.caseNumber;
        event.description = "event " + std::to_string(i);
        auto stored = repository.append(event);
        ASSERT_TRUE(stored.ok());
        EXPECT_EQ(stored.value().sequence, i + 1);
    }

    AuditQuery query;
    query.action = AuditAction::BookmarkCreated;
    auto filtered = repository.list(query);
    ASSERT_TRUE(filtered.ok());
    EXPECT_EQ(filtered.value().size(), 2u);

    AuditQuery textQuery;
    textQuery.text = "event 3";
    auto searched = repository.list(textQuery);
    ASSERT_TRUE(searched.ok());
    EXPECT_EQ(searched.value().size(), 1u);
}

TEST_F(RepositoryTest, ProvenanceLinksAssetsToOperations) {
    const Case owner = insertCase();
    const Evidence stored = insertEvidence(owner);
    ProvenanceRepository repository(database);

    ProcessingOperation operation;
    operation.id = generateUuid();
    operation.caseId = owner.id;
    operation.evidenceId = stored.id;
    operation.operationType = "frame_extraction";
    operation.parametersJson = R"({"method":"decode"})";
    operation.softwareName = "TRACE";
    operation.softwareVersion = "0.1.0";
    operation.sourceStartUs = 5'000'000;
    operation.startedAt = nowIso8601Utc();
    operation.performedBy = "tester";
    ASSERT_TRUE(repository.insertOperation(operation).ok());

    DerivedAsset asset;
    asset.id = generateUuid();
    asset.caseId = owner.id;
    asset.evidenceId = stored.id;
    asset.operationId = operation.id;
    asset.type = DerivedAssetType::FrameExport;
    asset.storageRelPath = owner.storageRelPath + "/exports/frame.png";
    asset.filename = "frame.png";
    asset.sha256 = "abc123";
    asset.sourceStartUs = 5'000'000;
    asset.createdAt = nowIso8601Utc();
    asset.createdBy = "tester";
    ASSERT_TRUE(repository.insertAsset(asset).ok());

    auto assets = repository.listAssetsForEvidence(stored.id);
    ASSERT_TRUE(assets.ok());
    ASSERT_EQ(assets.value().size(), 1u);
    EXPECT_EQ(assets.value().front().operationId.value_or(""), operation.id);
    EXPECT_EQ(assets.value().front().verificationState, VerificationState::Unverified)
        << "machine output must not be self-certified";

    auto operations = repository.listOperationsForEvidence(stored.id);
    ASSERT_TRUE(operations.ok());
    ASSERT_EQ(operations.value().size(), 1u);
    EXPECT_EQ(operations.value().front().sourceStartUs.value_or(0), 5'000'000);
}

TEST_F(RepositoryTest, SettingsAndUsersRoundTrip) {
    SettingsRepository settings(database);
    ASSERT_TRUE(settings.set("playback.default_speed", "1.5", "double", "tester").ok());
    ASSERT_TRUE(settings.set("playback.default_speed", "2.0", "double", "tester").ok());

    auto value = settings.get("playback.default_speed");
    ASSERT_TRUE(value.ok());
    ASSERT_TRUE(value.value().has_value());
    EXPECT_EQ(*value.value(), "2.0");

    auto missing = settings.get("nothing.here");
    ASSERT_TRUE(missing.ok());
    EXPECT_FALSE(missing.value().has_value());

    UserRepository users(database);
    UserAccount account;
    account.id = generateUuid();
    account.username = "analyst1";
    account.displayName = "Analyst One";
    account.role = UserRole::Analyst;
    auto storedUser = users.upsert(account);
    ASSERT_TRUE(storedUser.ok());
    EXPECT_FALSE(storedUser.value().lastSeenAt.empty());

    // Upserting the same username updates rather than duplicating.
    account.displayName = "Analyst One (day shift)";
    ASSERT_TRUE(users.upsert(account).ok());
    auto all = users.list();
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all.value().size(), 1u);
    EXPECT_EQ(all.value().front().displayName, "Analyst One (day shift)");
}

}  // namespace
}  // namespace trace
