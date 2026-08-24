// Phase 2 — exhibit bundles: writing them, verifying them, and refusing to present a
// failed export as a successful one.
#include <gtest/gtest.h>

#include <cstring>
#include <fstream>

#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/security/file_hasher.h"
#include "reporting/report_service.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

/// A case with one imported video, and the services a report needs.
struct Fixture {
    testing::TemporaryDirectory dataRoot{"trace-report"};
    testing::TestStack stack = testing::TestStack::create(dataRoot.path());
    Case caseRecord;
    Evidence evidence;
    std::filesystem::path source;

    Fixture() {
        const auto incoming = dataRoot.path() / "incoming";
        std::filesystem::create_directories(incoming);
        source = incoming / "sample.mp4";
        std::filesystem::copy_file(testing::sampleVideoPath(), source);

        CaseDraft draft;
        draft.caseNumber = "CASE-0001";
        draft.title = "Exhibit export";
        draft.investigator = "A. Analyst";
        caseRecord = stack.cases->createCase(draft).value();

        IngestRequest request;
        request.caseId = caseRecord.id;
        request.sourcePath = source;
        evidence = stack.evidence->ingest(request).value().evidence;
    }

    ReportService service() {
        return ReportService(*stack.layout, stack.database, stack.audit, *stack.cases,
                             *stack.evidence, *stack.analysis, *stack.annotations,
                             stack.derivedAssets);
    }

    ReportDraft baseDraft() {
        ReportDraft draft;
        draft.caseId = caseRecord.id;
        draft.title = "Findings exhibit";
        draft.evidenceIds = {evidence.id};
        return draft;
    }
};

std::int64_t countFiles(const std::filesystem::path& root) {
    std::int64_t n = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) ++n;
    }
    return n;
}

// ───────────────────────────────────────────────────────── writing a bundle

TEST(ReportExportTest, ExportsABundleWhoseEveryFileMatchesItsRecordedDigest) {
    Fixture fixture;
    auto service = fixture.service();

    auto draft = fixture.baseDraft();
    draft.frames.push_back({fixture.evidence.id, 1'000'000, "One second in"});
    draft.clips.push_back({fixture.evidence.id, 2'000'000, 4'000'000, "A two second range"});

    auto created = service.createReport(draft);
    ASSERT_TRUE(created.ok()) << created.error().message();

    const auto destination = fixture.dataRoot.path() / "out";
    std::filesystem::create_directories(destination);

    auto exported = service.exportReport(created.value().id, destination);
    ASSERT_TRUE(exported.ok()) << exported.error().message();
    const ExportOutcome outcome = exported.take();

    ASSERT_EQ(outcome.status, ReportStatus::Exported)
        << outcome.error.value_or("no error recorded");
    EXPECT_TRUE(outcome.selfCheck.passed())
        << "the export must verify its own output before claiming success";
    EXPECT_FALSE(outcome.manifestSha256.empty());
    EXPECT_GT(outcome.fileCount, 0);
    ASSERT_TRUE(std::filesystem::is_directory(outcome.bundlePath));

    // The three manifests, the report, the verification instructions.
    for (const auto& name : {"MANIFEST.json", "MANIFEST.checksums", "MANIFEST.sha256",
                             "REPORT.html", "VERIFY.md"}) {
        EXPECT_TRUE(std::filesystem::exists(outcome.bundlePath / name)) << name << " is missing";
    }
    for (const auto& name : {"provenance/evidence.json", "provenance/analysis_runs.json",
                             "provenance/detections.json", "audit/audit_extract.json",
                             "audit/audit_extract.csv"}) {
        EXPECT_TRUE(std::filesystem::exists(outcome.bundlePath / name)) << name << " is missing";
    }

    // Independently re-hash every listed file, exactly as a third party would.
    std::ifstream checksums(outcome.bundlePath / "MANIFEST.checksums");
    ASSERT_TRUE(checksums.is_open());
    std::string line;
    int checked = 0;
    while (std::getline(checksums, line)) {
        if (line.empty()) continue;
        ASSERT_GT(line.size(), 66u) << "malformed line: " << line;
        const std::string expected = line.substr(0, 64);
        const std::string relative = line.substr(66);
        auto actual = hashFile(outcome.bundlePath / relative);
        ASSERT_TRUE(actual.ok()) << relative;
        EXPECT_EQ(actual.value(), expected) << relative << " does not match the manifest";
        ++checked;
    }
    EXPECT_GT(checked, 0);
    EXPECT_EQ(checked, outcome.fileCount);

    // The exhibits were produced and are inside the bundle.
    EXPECT_EQ(countFiles(outcome.bundlePath / "exhibits"), 2);

    // The managed original is untouched by any of this.
    auto originalDigest = hashFile(fixture.stack.layout->resolve(fixture.evidence.storageRelPath));
    ASSERT_TRUE(originalDigest.ok());
    EXPECT_EQ(originalDigest.value(), fixture.evidence.sha256)
        << "exporting must not alter the managed original";
}

TEST(ReportExportTest, TheReportCarriesTheLimitationsStatementAndNoConclusion) {
    Fixture fixture;
    auto service = fixture.service();
    auto created = service.createReport(fixture.baseDraft());
    ASSERT_TRUE(created.ok());

    const auto destination = fixture.dataRoot.path() / "out";
    std::filesystem::create_directories(destination);
    auto exported = service.exportReport(created.value().id, destination);
    ASSERT_TRUE(exported.ok());
    const auto outcome = exported.take();
    ASSERT_EQ(outcome.status, ReportStatus::Exported);

    std::ifstream html(outcome.bundlePath / "REPORT.html");
    const std::string body((std::istreambuf_iterator<char>(html)),
                           std::istreambuf_iterator<char>());
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("they are not identifications and are not conclusions"),
              std::string::npos)
        << "every report must carry the limitations statement";

    // Wording TRACE is not entitled to use about evidence. Each of these can only ever
    // be a claim the software is not in a position to make.
    for (const char* banned : {"the suspect", "the offender", "proves", "court-admissible",
                               "forensically sound", "tamper-proof", "certified"}) {
        EXPECT_EQ(body.find(banned), std::string::npos)
            << "the report must never contain the phrase: " << banned;
    }

    // "signed" is different: the report is required to mention it, in the negative.
    // A manifest of digests is an integrity record, and calling it a signature would
    // overstate what the bundle proves — so the disclaimer must be present, and the
    // only place the word appears must be inside it.
    EXPECT_NE(body.find("Nothing here is digitally signed"), std::string::npos)
        << "the report must state that nothing in the bundle is signed";
    EXPECT_NE(body.find("integrity digests, not signatures"), std::string::npos);

    // And it must be the *only* mention: exactly one occurrence, inside the disclaimer.
    // A second one would almost certainly be a claim in the other direction.
    const std::size_t first = body.find("digitally signed");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(body.find("digitally signed", first + 1), std::string::npos)
        << "signing is mentioned more than once; the only mention must be the disclaimer";
    ASSERT_GE(first, std::strlen("Nothing here is "));
    EXPECT_EQ(body.substr(first - std::strlen("Nothing here is "), std::strlen("Nothing here is ")),
              "Nothing here is ")
        << "the mention of signing must be a denial, not a claim";
}

// ──────────────────────────────────────────────────────────── tamper tests

TEST(ReportExportTest, ChangingAnExhibitByOneByteFailsVerification) {
    Fixture fixture;
    auto service = fixture.service();
    auto draft = fixture.baseDraft();
    draft.frames.push_back({fixture.evidence.id, 500'000, "A frame"});

    auto created = service.createReport(draft);
    ASSERT_TRUE(created.ok());
    const auto destination = fixture.dataRoot.path() / "out";
    std::filesystem::create_directories(destination);
    auto exported = service.exportReport(created.value().id, destination);
    ASSERT_TRUE(exported.ok());
    const auto outcome = exported.take();
    ASSERT_EQ(outcome.status, ReportStatus::Exported);

    // Find the exhibit and flip one byte in it.
    std::filesystem::path exhibit;
    for (const auto& entry :
         std::filesystem::directory_iterator(outcome.bundlePath / "exhibits")) {
        exhibit = entry.path();
        break;
    }
    ASSERT_FALSE(exhibit.empty());
    {
        std::fstream file(exhibit, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekg(0, std::ios::end);
        const auto size = file.tellg();
        ASSERT_GT(size, 64);
        file.seekg(size / 2);
        char byte = 0;
        file.read(&byte, 1);
        file.seekp(size / 2);
        byte = static_cast<char>(byte ^ 0x01);
        file.write(&byte, 1);
    }

    auto verified = service.verifyBundle(outcome.bundlePath, fixture.caseRecord.id,
                                         fixture.caseRecord.caseNumber);
    ASSERT_TRUE(verified.ok());
    const BundleVerification& check = verified.value();

    EXPECT_TRUE(check.manifestIntact) << "the manifests themselves were not touched";
    EXPECT_FALSE(check.allFilesMatch);
    EXPECT_FALSE(check.passed());
    EXPECT_GE(check.failureCount(), 1);

    bool named = false;
    for (const auto& file : check.files) {
        if (!file.matches) {
            named = true;
            EXPECT_EQ(file.path, "exhibits/" + exhibit.filename().string());
            EXPECT_FALSE(file.problem.empty());
        }
    }
    EXPECT_TRUE(named) << "verification must name the file that failed";
}

TEST(ReportExportTest, EditingTheManifestIsCaughtByItsOwnDigest) {
    Fixture fixture;
    auto service = fixture.service();
    auto created = service.createReport(fixture.baseDraft());
    ASSERT_TRUE(created.ok());
    const auto destination = fixture.dataRoot.path() / "out";
    std::filesystem::create_directories(destination);
    auto exported = service.exportReport(created.value().id, destination);
    ASSERT_TRUE(exported.ok());
    const auto outcome = exported.take();
    ASSERT_EQ(outcome.status, ReportStatus::Exported);

    {
        std::ofstream manifest(outcome.bundlePath / "MANIFEST.checksums",
                               std::ios::app | std::ios::binary);
        manifest << "0000000000000000000000000000000000000000000000000000000000000000  fake.txt\n";
    }

    auto verified = BundleVerifier::verify(outcome.bundlePath);
    ASSERT_TRUE(verified.ok());
    EXPECT_FALSE(verified.value().manifestIntact)
        << "an edited MANIFEST.checksums must be caught by MANIFEST.sha256";
    EXPECT_FALSE(verified.value().passed());
    EXPECT_FALSE(verified.value().problem.empty());
}

TEST(ReportExportTest, AFilePlantedInTheBundleIsReportedAsUnlisted) {
    Fixture fixture;
    auto service = fixture.service();
    auto created = service.createReport(fixture.baseDraft());
    ASSERT_TRUE(created.ok());
    const auto destination = fixture.dataRoot.path() / "out";
    std::filesystem::create_directories(destination);
    auto exported = service.exportReport(created.value().id, destination);
    ASSERT_TRUE(exported.ok());
    const auto outcome = exported.take();
    ASSERT_EQ(outcome.status, ReportStatus::Exported);

    {
        std::ofstream planted(outcome.bundlePath / "exhibits" / "planted.txt");
        planted << "this was not exported by TRACE\n";
    }

    auto verified = BundleVerifier::verify(outcome.bundlePath);
    ASSERT_TRUE(verified.ok());
    EXPECT_TRUE(verified.value().manifestIntact);
    EXPECT_TRUE(verified.value().allFilesMatch) << "the listed files are all still intact";
    EXPECT_FALSE(verified.value().noUnlistedFiles);
    EXPECT_FALSE(verified.value().passed())
        << "a file nothing vouches for is a failure, not a curiosity";
    ASSERT_EQ(verified.value().unlistedFiles.size(), 1u);
    EXPECT_EQ(verified.value().unlistedFiles.front(), "exhibits/planted.txt");
}

// ─────────────────────────────────────────────── failure and cancellation

TEST(ReportExportTest, AFailedExportIsNeverRecordedAsExported) {
    Fixture fixture;
    auto service = fixture.service();
    auto created = service.createReport(fixture.baseDraft());
    ASSERT_TRUE(created.ok());

    // A destination inside a file, so the directory cannot be created.
    const auto blocker = fixture.dataRoot.path() / "not-a-directory";
    { std::ofstream(blocker) << "x"; }

    auto exported = service.exportReport(created.value().id, blocker / "nested");
    ASSERT_TRUE(exported.ok()) << "a failed export is an outcome, not a tool error";
    EXPECT_EQ(exported.value().status, ReportStatus::Failed);
    EXPECT_TRUE(exported.value().error.has_value());

    auto stored = service.findReport(created.value().id);
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    EXPECT_EQ(stored.value()->status, ReportStatus::Failed);
    EXPECT_NE(stored.value()->status, ReportStatus::Exported);
    EXPECT_FALSE(producedCompleteBundle(stored.value()->status));
    EXPECT_TRUE(stored.value()->errorMessage.has_value());
    EXPECT_FALSE(stored.value()->manifestSha256.has_value());
}

TEST(ReportExportTest, TheRepositoryRefusesToRecordAnExportedRunThatCarriesAnError) {
    Fixture fixture;
    ReportRepository repository(fixture.stack.database);

    Report report;
    report.id = generateUuid();
    report.caseId = fixture.caseRecord.id;
    report.title = "Rule check";
    report.createdAt = nowIso8601Utc();
    ASSERT_TRUE(repository.insertReport(report).ok());

    // exported + an error is a contradiction, and the storage layer says so.
    auto contradiction = repository.finishExport(report.id, ReportStatus::Exported,
                                                 nowIso8601Utc(), "bundle", "digest", "{}",
                                                 "something went wrong");
    EXPECT_FALSE(contradiction.ok());

    // A non-terminal state is not an outcome.
    auto notTerminal = repository.finishExport(report.id, ReportStatus::Exporting, nowIso8601Utc());
    EXPECT_FALSE(notTerminal.ok());

    // Exported without a bundle to point at is equally meaningless.
    auto noBundle = repository.finishExport(report.id, ReportStatus::Exported, nowIso8601Utc());
    EXPECT_FALSE(noBundle.ok());
}

TEST(ReportExportTest, CancellingLeavesNoBundlePresentedAsComplete) {
    Fixture fixture;
    auto service = fixture.service();
    auto draft = fixture.baseDraft();
    draft.frames.push_back({fixture.evidence.id, 1'000'000, "A frame"});
    auto created = service.createReport(draft);
    ASSERT_TRUE(created.ok());

    const auto destination = fixture.dataRoot.path() / "out";
    std::filesystem::create_directories(destination);

    auto token = std::make_shared<std::atomic<bool>>(false);
    // Cancel at the first progress report, which happens before anything is written.
    auto exported = service.exportReport(
        created.value().id, destination,
        [&](const ExportProgress&) {
            token->store(true);
            return false;
        },
        token);

    ASSERT_TRUE(exported.ok());
    EXPECT_EQ(exported.value().status, ReportStatus::Cancelled);
    EXPECT_NE(exported.value().status, ReportStatus::Exported);
    EXPECT_FALSE(std::filesystem::exists(exported.value().bundlePath))
        << "a cancelled export must not leave a partial bundle behind";

    auto stored = service.findReport(created.value().id);
    ASSERT_TRUE(stored.ok());
    EXPECT_EQ(stored.value()->status, ReportStatus::Cancelled);
    EXPECT_FALSE(producedCompleteBundle(stored.value()->status));
}

TEST(ReportExportTest, VerifyInstructionsTellAReaderHowToDetectAnAddedFile) {
    // The bundle's own instructions have to be sufficient. A digest check cannot catch
    // a file that was added — it is not on the list to be checked — so VERIFY.md must
    // carry a count comparison as well, or a reader following it exactly would miss the
    // one kind of tampering sha256sum is blind to.
    const std::string document = BundleVerifier::verifyDocument();

    EXPECT_NE(document.find("sha256sum -c MANIFEST.sha256"), std::string::npos);
    EXPECT_NE(document.find("sha256sum -c MANIFEST.checksums"), std::string::npos);

    EXPECT_NE(document.find("wc -l < MANIFEST.checksums"), std::string::npos)
        << "VERIFY.md must show the reader how to count what the manifest lists";
    EXPECT_NE(document.find("find . -type f"), std::string::npos)
        << "VERIFY.md must show the reader how to count what is actually present";
    EXPECT_NE(document.find("has been *added*"), std::string::npos)
        << "VERIFY.md must say plainly why the digest checks are not sufficient alone";

    // And it must not overstate what any of it proves.
    EXPECT_NE(document.find("not a digital signature"), std::string::npos);
    EXPECT_NE(document.find("not who wrote it"), std::string::npos);
}

TEST(ReportExportTest, TheDocumentedCountCheckActuallyCatchesAPlantedFile) {
    // Run the arithmetic VERIFY.md asks a reader to run, and confirm it disagrees
    // exactly when a file has been planted.
    Fixture fixture;
    auto service = fixture.service();
    auto created = service.createReport(fixture.baseDraft());
    ASSERT_TRUE(created.ok());
    const auto destination = fixture.dataRoot.path() / "out";
    std::filesystem::create_directories(destination);
    auto exported = service.exportReport(created.value().id, destination);
    ASSERT_TRUE(exported.ok());
    const auto outcome = exported.take();
    ASSERT_EQ(outcome.status, ReportStatus::Exported);

    const auto countPresent = [&] {
        int n = 0;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(outcome.bundlePath)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().filename().string().rfind("MANIFEST.", 0) == 0) continue;
            ++n;
        }
        return n;
    };
    const auto countListed = [&] {
        std::ifstream listing(outcome.bundlePath / "MANIFEST.checksums");
        int n = 0;
        std::string line;
        while (std::getline(listing, line)) {
            if (!line.empty()) ++n;
        }
        return n;
    };

    EXPECT_EQ(countPresent(), countListed()) << "a freshly written bundle must agree";

    { std::ofstream(outcome.bundlePath / "exhibits" / "planted.txt") << "added later\n"; }

    EXPECT_NE(countPresent(), countListed())
        << "the count comparison must notice a planted file";
    EXPECT_EQ(countPresent(), countListed() + 1);

    // And every listed digest still matches, which is precisely why the count is needed.
    auto verified = BundleVerifier::verify(outcome.bundlePath);
    ASSERT_TRUE(verified.ok());
    EXPECT_TRUE(verified.value().allFilesMatch)
        << "the digest checks alone are blind to an added file";
    EXPECT_FALSE(verified.value().passed());
}

// ─────────────────────────────────────────────────── the confirmed-only rule

TEST(ReportExportTest, CitingAnUnconfirmedDetectionWithoutOptingInFailsTheExport) {
    Fixture fixture;

    // Store one unreviewed detection directly; no model is needed to test the rule.
    AnalysisRunDraft runDraft;
    runDraft.caseId = fixture.caseRecord.id;
    runDraft.caseNumber = fixture.caseRecord.caseNumber;
    runDraft.evidenceId = fixture.evidence.id;
    runDraft.evidenceNumber = fixture.evidence.evidenceNumber;
    runDraft.providerName = "mock";
    runDraft.modelName = "none";
    runDraft.evidenceSha256 = fixture.evidence.sha256;
    auto run = fixture.stack.analysis->startRun(runDraft);
    ASSERT_TRUE(run.ok());

    Detection detection;
    detection.id = generateUuid();
    detection.analysisRunId = run.value().id;
    detection.caseId = fixture.caseRecord.id;
    detection.evidenceId = fixture.evidence.id;
    detection.timestampUs = 1'000'000;
    detection.classLabel = "person";
    detection.classGroup = DetectionClassGroup::Person;
    detection.confidence = 0.9;
    detection.box = NormalizedBox{0.1, 0.1, 0.2, 0.4};
    detection.verification = DetectionVerification::Unreviewed;
    detection.createdAt = nowIso8601Utc();
    ASSERT_TRUE(fixture.stack.analysis->storeDetections({detection}).ok());

    auto service = fixture.service();
    auto draft = fixture.baseDraft();
    draft.detectionIds = {detection.id};
    draft.includeUnconfirmedDetections = false;  // the default

    auto created = service.createReport(draft);
    ASSERT_TRUE(created.ok());
    const auto destination = fixture.dataRoot.path() / "out";
    std::filesystem::create_directories(destination);

    auto exported = service.exportReport(created.value().id, destination);
    ASSERT_TRUE(exported.ok());
    EXPECT_EQ(exported.value().status, ReportStatus::Failed)
        << "an unconfirmed detection must not reach a report that did not opt in";
    ASSERT_TRUE(exported.value().error.has_value());
    EXPECT_NE(exported.value().error->find("confirmed"), std::string::npos);
}

TEST(ReportExportTest, OptingInSaysSoInTheReportItself) {
    Fixture fixture;

    AnalysisRunDraft runDraft;
    runDraft.caseId = fixture.caseRecord.id;
    runDraft.caseNumber = fixture.caseRecord.caseNumber;
    runDraft.evidenceId = fixture.evidence.id;
    runDraft.evidenceNumber = fixture.evidence.evidenceNumber;
    runDraft.providerName = "mock";
    runDraft.modelName = "none";
    runDraft.evidenceSha256 = fixture.evidence.sha256;
    auto run = fixture.stack.analysis->startRun(runDraft);
    ASSERT_TRUE(run.ok());

    Detection detection;
    detection.id = generateUuid();
    detection.analysisRunId = run.value().id;
    detection.caseId = fixture.caseRecord.id;
    detection.evidenceId = fixture.evidence.id;
    detection.timestampUs = 1'000'000;
    detection.classLabel = "person";
    detection.confidence = 0.9;
    detection.box = NormalizedBox{0.1, 0.1, 0.2, 0.4};
    detection.verification = DetectionVerification::Unreviewed;
    detection.createdAt = nowIso8601Utc();
    ASSERT_TRUE(fixture.stack.analysis->storeDetections({detection}).ok());

    auto service = fixture.service();
    auto draft = fixture.baseDraft();
    draft.detectionIds = {detection.id};
    draft.includeUnconfirmedDetections = true;

    auto created = service.createReport(draft);
    ASSERT_TRUE(created.ok());
    const auto destination = fixture.dataRoot.path() / "out";
    std::filesystem::create_directories(destination);
    auto exported = service.exportReport(created.value().id, destination);
    ASSERT_TRUE(exported.ok()) << exported.error().message();
    ASSERT_EQ(exported.value().status, ReportStatus::Exported)
        << exported.value().error.value_or("");

    std::ifstream html(exported.value().bundlePath / "REPORT.html");
    const std::string body((std::istreambuf_iterator<char>(html)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("includes detections that no analyst has confirmed"), std::string::npos)
        << "a report containing unreviewed detections must say so prominently";
    EXPECT_NE(body.find("UNREVIEWED"), std::string::npos)
        << "the review state of an unconfirmed detection must be shown against it";
}

// ──────────────────────────────────────────────────────────────── restart

TEST(ReportExportTest, ReportsAndTheirBundlesSurviveARestart) {
    testing::TemporaryDirectory dataRoot("trace-report-restart");
    std::string reportId;
    std::string manifestDigest;
    std::filesystem::path bundlePath;
    std::string caseId;

    {
        Fixture fixture;
        // Re-point the fixture's own root so the second session opens the same database.
        (void)dataRoot;
        auto service = fixture.service();
        auto created = service.createReport(fixture.baseDraft());
        ASSERT_TRUE(created.ok());
        const auto destination = fixture.dataRoot.path() / "out";
        std::filesystem::create_directories(destination);
        auto exported = service.exportReport(created.value().id, destination);
        ASSERT_TRUE(exported.ok());
        ASSERT_EQ(exported.value().status, ReportStatus::Exported);

        reportId = created.value().id;
        manifestDigest = exported.value().manifestSha256;
        bundlePath = exported.value().bundlePath;
        caseId = fixture.caseRecord.id;

        // The bundle re-verifies from disk while the first session is still open.
        auto verified = BundleVerifier::verify(bundlePath);
        ASSERT_TRUE(verified.ok());
        EXPECT_TRUE(verified.value().passed());
        EXPECT_EQ(verified.value().manifestSha256, manifestDigest);

        // Reopening the database in the same directory finds the report again.
        auto reopened = testing::TestStack::create(fixture.dataRoot.path());
        ReportRepository repository(reopened.database);
        auto stored = repository.findReport(reportId);
        ASSERT_TRUE(stored.ok());
        ASSERT_TRUE(stored.value().has_value());
        EXPECT_EQ(stored.value()->status, ReportStatus::Exported);
        EXPECT_EQ(stored.value()->manifestSha256.value_or(""), manifestDigest);
        EXPECT_FALSE(stored.value()->itemCountsJson.empty());

        auto items = repository.itemsFor(reportId);
        ASSERT_TRUE(items.ok());
        EXPECT_FALSE(items.value().empty());
    }
}

}  // namespace
}  // namespace trace
