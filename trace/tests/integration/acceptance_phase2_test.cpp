// TRACE Phase 2 acceptance test.
//
// Drives the real application — ApplicationContext, MainWindow, the report builder
// dialog and the export that runs behind it — through writing an exhibit bundle, then
// verifies that bundle the way a third party would: by re-hashing every file listed in
// the manifest, using nothing the application produced at display time.
//
// Three workflows:
//   §1 a successful export, verified independently, surviving a restart
//   §2 a failed export: recorded failed, never exported, no bundle left behind
//   §3 a cancelled export: nothing presented as complete
#include <algorithm>
#include <gtest/gtest.h>
#include <iterator>

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>

#include "core/common/uuid.h"
#include "core/security/file_hasher.h"
#include "tests/support/test_environment.h"
#include "ui/app/application_context.h"
#include "ui/app/main_window.h"
#include "ui/common/theme.h"
#include "ui/evidence_browser/evidence_panel.h"
#include "ui/reports/report_builder_dialog.h"

namespace trace {
namespace {

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) return true;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

/// Dismisses any modal box so an unattended run cannot deadlock on one.
class MessageBoxCollector : public QObject {
public:
    MessageBoxCollector() {
        timer_ = new QTimer(this);
        timer_->setInterval(25);
        connect(timer_, &QTimer::timeout, this, [this] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (box == nullptr) return;
            lastText_ = box->text();
            ++count_;
            box->accept();
        });
        timer_->start();
    }
    QString lastText() const { return lastText_; }
    int count() const { return count_; }

private:
    QTimer* timer_ = nullptr;
    QString lastText_;
    int count_ = 0;
};

struct Workspace {
    std::unique_ptr<testing::TemporaryDirectory> dataRoot;
    std::unique_ptr<MessageBoxCollector> messageBoxes;
    std::unique_ptr<ui::ApplicationContext> context;
    std::unique_ptr<ui::MainWindow> window;
    Evidence evidence;
    std::string caseId;

    static std::unique_ptr<Workspace> open(const std::string& prefix) {
        auto workspace = std::make_unique<Workspace>();
        workspace->messageBoxes = std::make_unique<MessageBoxCollector>();
        workspace->dataRoot = std::make_unique<testing::TemporaryDirectory>(prefix);

        const auto incoming = workspace->dataRoot->path() / "incoming";
        std::filesystem::create_directories(incoming);
        const auto source = incoming / "sample.mp4";
        std::filesystem::copy_file(testing::sampleVideoPath(), source);

        workspace->context = std::make_unique<ui::ApplicationContext>();
        if (!workspace->context->initialise(workspace->dataRoot->path())) return nullptr;

        CaseDraft draft;
        draft.caseNumber = "CASE-0001";
        draft.title = "Exhibit acceptance";
        draft.investigator = "A. Analyst";
        auto created = workspace->context->cases().createCase(draft);
        if (!created) return nullptr;
        workspace->caseId = created.value().id;
        if (!workspace->context->openCase(QString::fromStdString(workspace->caseId))) return nullptr;

        IngestRequest request;
        request.caseId = workspace->caseId;
        request.sourcePath = source;
        auto ingested = workspace->context->evidence().ingest(request);
        if (!ingested) return nullptr;
        workspace->evidence = ingested.value().evidence;

        workspace->window = std::make_unique<ui::MainWindow>(workspace->context.get());
        workspace->window->show();
        if (!waitFor([&] { return workspace->window->isVisible(); }, 10000)) return nullptr;

        workspace->window->evidencePanel()->refresh();
        workspace->window->evidencePanel()->selectEvidence(
            QString::fromStdString(workspace->evidence.id));
        if (!waitFor([&] { return workspace->context->currentEvidence().has_value(); }, 10000)) {
            return nullptr;
        }
        return workspace;
    }

    ~Workspace() {
        if (window != nullptr) {
            window->close();
            QApplication::processEvents();
        }
    }
};

/// Checks a bundle the way someone with no TRACE would: read MANIFEST.checksums,
/// hash each file, compare. Nothing here calls the application's own verifier.
int verifyIndependently(const std::filesystem::path& bundle, int* mismatches) {
    *mismatches = 0;
    std::ifstream listing(bundle / "MANIFEST.checksums");
    if (!listing.is_open()) return -1;

    int checked = 0;
    std::string line;
    while (std::getline(listing, line)) {
        if (line.size() < 67) continue;
        const std::string expected = line.substr(0, 64);
        const std::string relative = line.substr(66);
        auto actual = hashFile(bundle / relative);
        if (!actual || actual.value() != expected) ++(*mismatches);
        ++checked;
    }
    return checked;
}

// ═══════════════════════════════════════════════════════ §1 successful export

TEST(AcceptancePhase2, ExportsABundleThatVerifiesIndependentlyAndSurvivesARestart) {
    auto workspace = Workspace::open("trace-phase2-accept");
    ASSERT_NE(workspace, nullptr);

    const auto destination = workspace->dataRoot->path() / "bundles";
    std::filesystem::create_directories(destination);

    auto* dialog = workspace->window->exportExhibitBundle();
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(waitFor([&] { return dialog->isVisible(); }, 10000));

    // The evidence in the viewer is offered pre-selected; the operator names the report.
    dialog->titleField()->setText(QStringLiteral("Findings for review"));
    ASSERT_EQ(dialog->evidenceTree()->topLevelItemCount(), 1);
    dialog->evidenceTree()->topLevelItem(0)->setCheckState(0, Qt::Checked);
    QApplication::processEvents();
    ASSERT_TRUE(dialog->exportButton()->isEnabled());

    // Unconfirmed detections are off by default — this is the safe setting.
    EXPECT_FALSE(dialog->unconfirmedBox()->isChecked());

    dialog->setDestinationOverride(QString::fromStdString(destination.string()));
    dialog->exportButton()->click();
    ASSERT_TRUE(waitFor([&] { return !dialog->exportRunning(); }, 120000))
        << "the export never finished";
    QApplication::processEvents();

    ASSERT_TRUE(dialog->outcome().has_value());
    const ExportOutcome outcome = *dialog->outcome();
    ASSERT_EQ(outcome.status, ReportStatus::Exported)
        << outcome.error.value_or("no error recorded");
    EXPECT_TRUE(outcome.selfCheck.passed());
    ASSERT_TRUE(std::filesystem::is_directory(outcome.bundlePath));

    // A paginated copy is produced through the Qt renderer the desktop app installs.
    EXPECT_TRUE(std::filesystem::exists(outcome.bundlePath / "REPORT.pdf"))
        << "the desktop application should render a PDF alongside the HTML";

    // ---- verified the way a third party would, with no help from TRACE ----
    int mismatches = 0;
    const int checked = verifyIndependently(outcome.bundlePath, &mismatches);
    EXPECT_GT(checked, 0);
    EXPECT_EQ(mismatches, 0) << "a freshly exported bundle must verify from its own manifest";
    EXPECT_EQ(checked, outcome.fileCount);

    // MANIFEST.sha256 covers the manifests themselves.
    {
        std::ifstream top(outcome.bundlePath / "MANIFEST.sha256");
        std::string line;
        int manifestsChecked = 0;
        while (std::getline(top, line)) {
            if (line.size() < 67) continue;
            auto actual = hashFile(outcome.bundlePath / line.substr(66));
            ASSERT_TRUE(actual.ok());
            EXPECT_EQ(actual.value(), line.substr(0, 64)) << line.substr(66);
            ++manifestsChecked;
        }
        EXPECT_EQ(manifestsChecked, 2) << "both manifests must be covered";
    }

    // The instructions a reader follows are in the bundle.
    {
        std::ifstream verify(outcome.bundlePath / "VERIFY.md");
        const std::string text((std::istreambuf_iterator<char>(verify)),
                               std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("sha256sum -c MANIFEST.checksums"), std::string::npos);
        EXPECT_NE(text.find("not a digital signature"), std::string::npos);
    }

    // The managed original is untouched.
    auto digest = hashFile(workspace->context->layout().resolve(workspace->evidence.storageRelPath));
    ASSERT_TRUE(digest.ok());
    EXPECT_EQ(digest.value(), workspace->evidence.sha256);

    // The audit trail records the export.
    {
        AuditQuery query;
        query.caseId = workspace->caseId;
        query.limit = 500;
        auto events = workspace->context->audit().list(query);
        ASSERT_TRUE(events.ok());
        const auto has = [&](AuditAction action) {
            return std::any_of(events.value().begin(), events.value().end(),
                               [&](const AuditEvent& e) { return e.action == action; });
        };
        EXPECT_TRUE(has(AuditAction::ReportCreated));
        EXPECT_TRUE(has(AuditAction::ReportExported));
        EXPECT_FALSE(has(AuditAction::ReportExportFailed));
    }

    const auto bundlePath = outcome.bundlePath;
    const auto manifestDigest = outcome.manifestSha256;
    const auto dataRootPath = workspace->dataRoot->path();
    auto keepDirectory = std::move(workspace->dataRoot);
    dialog->close();
    workspace.reset();
    QApplication::processEvents();

    // ══════════════════════════ RESTART ══════════════════════════
    {
        ui::ApplicationContext context;
        ASSERT_TRUE(context.initialise(dataRootPath).ok());

        auto cases = context.cases().listCases();
        ASSERT_TRUE(cases.ok());
        ASSERT_EQ(cases.value().size(), 1u);
        auto reports = context.reports().listForCase(cases.value().front().id);
        ASSERT_TRUE(reports.ok());
        ASSERT_EQ(reports.value().size(), 1u);

        const Report stored = reports.value().front();
        EXPECT_EQ(stored.status, ReportStatus::Exported);
        EXPECT_EQ(stored.manifestSha256.value_or(""), manifestDigest);
        EXPECT_TRUE(stored.bundleRelPath.has_value());
        EXPECT_FALSE(stored.includedUnconfirmed);

        // And the bundle on disk still verifies, through the application this time.
        auto verified = context.reports().verifyBundle(bundlePath, cases.value().front().id,
                                                       cases.value().front().caseNumber);
        ASSERT_TRUE(verified.ok());
        EXPECT_TRUE(verified.value().passed());
        EXPECT_EQ(verified.value().manifestSha256, manifestDigest);
    }
}

// ═════════════════════════════════════════════════════════ §2 failed export

TEST(AcceptancePhase2, AFailedExportIsRecordedFailedAndLeavesNoBundle) {
    auto workspace = Workspace::open("trace-phase2-fail");
    ASSERT_NE(workspace, nullptr);

    // A destination inside a regular file: the bundle directory cannot be created.
    const auto blocker = workspace->dataRoot->path() / "blocked";
    { std::ofstream(blocker) << "not a directory"; }

    auto* dialog = workspace->window->exportExhibitBundle();
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(waitFor([&] { return dialog->isVisible(); }, 10000));
    dialog->evidenceTree()->topLevelItem(0)->setCheckState(0, Qt::Checked);
    QApplication::processEvents();

    dialog->setDestinationOverride(QString::fromStdString((blocker / "inside").string()));
    dialog->exportButton()->click();
    ASSERT_TRUE(waitFor([&] { return !dialog->exportRunning(); }, 60000));
    QApplication::processEvents();

    ASSERT_TRUE(dialog->outcome().has_value());
    EXPECT_EQ(dialog->outcome()->status, ReportStatus::Failed);
    EXPECT_NE(dialog->outcome()->status, ReportStatus::Exported);
    EXPECT_TRUE(dialog->outcome()->error.has_value());
    EXPECT_FALSE(std::filesystem::exists(dialog->outcome()->bundlePath));

    // The operator is told, in a box they must acknowledge.
    EXPECT_GT(workspace->messageBoxes->count(), 0)
        << "a failed export must be reported, not swallowed";

    auto reports = workspace->context->reports().listForCase(workspace->caseId);
    ASSERT_TRUE(reports.ok());
    ASSERT_EQ(reports.value().size(), 1u);
    EXPECT_EQ(reports.value().front().status, ReportStatus::Failed);
    EXPECT_FALSE(producedCompleteBundle(reports.value().front().status));
    EXPECT_FALSE(reports.value().front().manifestSha256.has_value());

    AuditQuery query;
    query.caseId = workspace->caseId;
    query.limit = 500;
    auto events = workspace->context->audit().list(query);
    ASSERT_TRUE(events.ok());
    const auto count = [&](AuditAction action) {
        return std::count_if(events.value().begin(), events.value().end(),
                             [&](const AuditEvent& e) { return e.action == action; });
    };
    EXPECT_GT(count(AuditAction::ReportExportFailed), 0);
    EXPECT_EQ(count(AuditAction::ReportExported), 0);

    dialog->close();
}

// ════════════════════════════════════════════════════ §3 cancelled export

TEST(AcceptancePhase2, CancellingLeavesNothingPresentedAsComplete) {
    auto workspace = Workspace::open("trace-phase2-cancel");
    ASSERT_NE(workspace, nullptr);

    const auto destination = workspace->dataRoot->path() / "bundles";
    std::filesystem::create_directories(destination);

    auto* dialog = workspace->window->exportExhibitBundle();
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(waitFor([&] { return dialog->isVisible(); }, 10000));
    dialog->evidenceTree()->topLevelItem(0)->setCheckState(0, Qt::Checked);
    QApplication::processEvents();

    // Cancel is armed the moment the export commits, not once a worker happens to
    // start — the Phase 1 defect this repeats the guard against.
    dialog->setDestinationOverride(QString::fromStdString(destination.string()));
    dialog->exportButton()->click();
    EXPECT_TRUE(dialog->exportRunning());
    ASSERT_TRUE(dialog->cancelButton()->isEnabled())
        << "the operator must be able to stop an export the instant it starts";
    dialog->cancelButton()->click();

    ASSERT_TRUE(waitFor([&] { return !dialog->exportRunning(); }, 60000));
    QApplication::processEvents();

    ASSERT_TRUE(dialog->outcome().has_value());
    EXPECT_NE(dialog->outcome()->status, ReportStatus::Exported)
        << "a cancelled export must never be recorded as exported";
    EXPECT_FALSE(std::filesystem::exists(dialog->outcome()->bundlePath))
        << "cancelling must not leave a partial bundle behind";

    auto reports = workspace->context->reports().listForCase(workspace->caseId);
    ASSERT_TRUE(reports.ok());
    ASSERT_EQ(reports.value().size(), 1u);
    EXPECT_FALSE(producedCompleteBundle(reports.value().front().status));

    dialog->close();
}

}  // namespace
}  // namespace trace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    trace::ui::applyTraceTheme(application);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
