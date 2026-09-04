// Service-level behaviour: validation, audit side effects and the analysis
// boundary.
#include <gtest/gtest.h>

#include "ai/interfaces/provider_registry.h"
#include "core/common/time_utils.h"
#include "core/settings/settings_service.h"
#include "core/settings/system_capabilities.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

class ServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory = std::make_unique<testing::TemporaryDirectory>("trace-service");
        stack = std::make_unique<testing::TestStack>(
            testing::TestStack::create(directory->path(), false));
    }

    std::unique_ptr<testing::TemporaryDirectory> directory;
    std::unique_ptr<testing::TestStack> stack;
};

TEST_F(ServiceTest, CreatingACaseMakesStorageAndAnAuditRecord) {
    CaseDraft draft;
    draft.title = "Sample Incident";
    draft.investigator = "A. Analyst";
    draft.agency = "Example Agency";

    auto created = stack->cases->createCase(draft);
    ASSERT_TRUE(created.ok()) << created.error().toString();
    const Case value = created.take();

    EXPECT_EQ(value.caseNumber, "CASE-0001");
    EXPECT_FALSE(value.id.empty());
    EXPECT_EQ(value.storageRelPath, "cases/" + value.id);
    EXPECT_TRUE(std::filesystem::exists(stack->layout->originalsDirectory(value.id)));
    EXPECT_FALSE(value.createdBy.empty());

    AuditQuery query;
    query.action = AuditAction::CaseCreated;
    auto events = stack->audit->list(query);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1u);
    EXPECT_EQ(events.value().front().caseNumber.value_or(""), "CASE-0001");
    EXPECT_NE(events.value().front().detailsJson.find("Sample Incident"), std::string::npos);
}

TEST_F(ServiceTest, CaseCreationRejectsMissingTitleAndDuplicateNumbers) {
    CaseDraft empty;
    auto failed = stack->cases->createCase(empty);
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.error().code(), ErrorCode::InvalidArgument);

    CaseDraft first;
    first.caseNumber = "CASE-0001";
    first.title = "First";
    ASSERT_TRUE(stack->cases->createCase(first).ok());

    CaseDraft duplicate;
    duplicate.caseNumber = "CASE-0001";
    duplicate.title = "Second";
    auto rejected = stack->cases->createCase(duplicate);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code(), ErrorCode::AlreadyExists);
}

TEST_F(ServiceTest, OpeningACaseIsRecorded) {
    CaseDraft draft;
    draft.title = "Sample Incident";
    auto created = stack->cases->createCase(draft);
    ASSERT_TRUE(created.ok());

    auto opened = stack->cases->openCase(created.value().id);
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(opened.value().caseNumber, "CASE-0001");

    AuditQuery query;
    query.action = AuditAction::CaseOpened;
    auto events = stack->audit->list(query);
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events.value().size(), 1u);

    auto missing = stack->cases->openCase("00000000-0000-4000-8000-000000000000");
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST_F(ServiceTest, EditingACaseRecordsWhatChanged) {
    CaseDraft draft;
    draft.title = "Sample Incident";
    auto created = stack->cases->createCase(draft);
    ASSERT_TRUE(created.ok());

    Case updated = created.take();
    updated.title = "Sample Incident (revised)";
    updated.status = CaseStatus::Closed;
    ASSERT_TRUE(stack->cases->updateCase(updated).ok());

    AuditQuery query;
    query.action = AuditAction::CaseStatusChanged;
    auto events = stack->audit->list(query);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1u);
    const std::string details = events.value().front().detailsJson;
    EXPECT_NE(details.find("\"from\":\"open\""), std::string::npos);
    EXPECT_NE(details.find("\"to\":\"closed\""), std::string::npos);

    Case invalid = updated;
    invalid.title = "   ";
    EXPECT_FALSE(stack->cases->updateCase(invalid).ok());
}

TEST_F(ServiceTest, BookmarkAndAnnotationValidationAndAudit) {
    CaseDraft draft;
    draft.title = "Sample Incident";
    auto created = stack->cases->createCase(draft);
    ASSERT_TRUE(created.ok());
    const Case owner = created.take();

    // A bookmark must belong to an evidence item.
    BookmarkDraft orphan;
    orphan.timestampUs = 1'000'000;
    auto rejected = stack->annotations->createBookmark(orphan, owner.caseNumber, "EVD-000001");
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);

    // Annotations must carry text, and cannot end before they start.
    AnnotationDraft inverted;
    inverted.caseId = owner.id;
    inverted.evidenceId = "some-evidence";
    inverted.startUs = 5'000'000;
    inverted.endUs = 1'000'000;
    inverted.text = "backwards";
    auto invertedResult =
        stack->annotations->createAnnotation(inverted, owner.caseNumber, "EVD-000001");
    ASSERT_FALSE(invertedResult.ok());
    EXPECT_EQ(invertedResult.error().code(), ErrorCode::InvalidArgument);

    AnnotationDraft empty;
    empty.caseId = owner.id;
    empty.evidenceId = "some-evidence";
    empty.startUs = 1'000'000;
    auto emptyResult = stack->annotations->createAnnotation(empty, owner.caseNumber, "EVD-000001");
    ASSERT_FALSE(emptyResult.ok());
}

TEST_F(ServiceTest, SettingsHaveDefaultsAndRecordChanges) {
    SettingsService settings(stack->database, stack->audit);
    ASSERT_TRUE(settings.ensureDefaults().ok());

    EXPECT_DOUBLE_EQ(settings.getDouble(settings_keys::kPlaybackDefaultSpeed, 0.0), 1.0);
    EXPECT_EQ(settings.getInt(settings_keys::kPlaybackJumpSeconds, 0), 5);
    EXPECT_FALSE(settings.getBool(settings_keys::kHardwareAcceleration, true));
    EXPECT_EQ(settings.logLevel(), LogLevel::Info);

    ASSERT_TRUE(settings.setInt(settings_keys::kPlaybackJumpSeconds, 10).ok());
    EXPECT_EQ(settings.getInt(settings_keys::kPlaybackJumpSeconds, 0), 10);

    AuditQuery query;
    query.action = AuditAction::SettingsChanged;
    auto events = stack->audit->list(query);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1u);
    EXPECT_NE(events.value().front().detailsJson.find("\"previous_value\":\"5\""), std::string::npos);
    EXPECT_NE(events.value().front().detailsJson.find("\"new_value\":\"10\""), std::string::npos);
}

TEST_F(ServiceTest, ReservedSettingsAreMarkedInactiveForThisPhase) {
    const auto* hardware = findSettingDefinition(settings_keys::kHardwareAcceleration);
    ASSERT_NE(hardware, nullptr);
    EXPECT_FALSE(hardware->activeInPhase0)
        << "settings the phase does not honour must be declared inactive";

    const auto* device = findSettingDefinition(settings_keys::kAnalysisDevice);
    ASSERT_NE(device, nullptr);
    EXPECT_FALSE(device->activeInPhase0);

    const auto* speed = findSettingDefinition(settings_keys::kPlaybackDefaultSpeed);
    ASSERT_NE(speed, nullptr);
    EXPECT_TRUE(speed->activeInPhase0);
}

TEST_F(ServiceTest, SystemCapabilitiesAreDetectedNotAssumed) {
    const SystemCapabilities capabilities = detectSystemCapabilities(directory->path().string());
    EXPECT_GT(capabilities.logicalCores, 0u);
    EXPECT_FALSE(capabilities.operatingSystem.empty());
    EXPECT_GT(capabilities.dataDiskTotalBytes, 0);
}

TEST(AnalysisBoundary, RegistryIsEmptyInPhaseZeroButAcceptsProviders) {
    auto& registry = AnalysisProviderRegistry::instance();
    EXPECT_TRUE(registry.empty()) << "Phase 0 must ship no analysis providers";

    struct StubProvider : IAnalysisProvider {
        std::string name() const override { return "stub"; }
        std::string version() const override { return "0.0.1"; }
        AnalysisProviderCapabilities capabilities() const override {
            AnalysisProviderCapabilities capabilities;
            capabilities.analysisTypes = {"detection"};
            return capabilities;
        }
        bool isAvailable() const override { return true; }
        Result<AnalysisResult> analyze(const AnalysisRequest& request) override {
            AnalysisResult result;
            result.requestId = request.requestId;
            result.status = AnalysisStatus::Completed;
            return Result<AnalysisResult>::success(std::move(result));
        }
    };

    auto provider = std::make_shared<StubProvider>();
    ASSERT_TRUE(registry.registerProvider(provider).ok());
    EXPECT_FALSE(registry.registerProvider(provider).ok()) << "names must be unique";
    EXPECT_EQ(registry.providersFor("detection").size(), 1u);
    EXPECT_EQ(registry.providersFor("transcription").size(), 0u);
    EXPECT_TRUE(registry.unregisterProvider("stub"));
    EXPECT_TRUE(registry.empty());
}

}  // namespace
}  // namespace trace
