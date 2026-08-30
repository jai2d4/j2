// TRACE Phase 0 acceptance test (§37).
//
// This drives the real application — the same ApplicationContext, MainWindow,
// panels, decoder and database that an operator uses — through the whole
// documented workflow, then restarts it and checks that everything survived.
// Only the file-chooser dialog is bypassed: the import runs through the real
// IngestDialog with the path supplied directly.
#include <algorithm>
#include <gtest/gtest.h>

#include <QApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QThread>
#include <QWidget>

#include <functional>
#include <iostream>
#include <string>

#include "core/security/file_hasher.h"
#include "tests/support/test_environment.h"
#include "ui/annotations/bookmark_dialog.h"
#include "ui/annotations/bookmarks_panel.h"
#include "ui/app/application_context.h"
#include "ui/app/main_window.h"
#include "ui/audit_viewer/audit_panel.h"
#include "ui/case_browser/case_browser_panel.h"
#include "ui/case_browser/case_dialog.h"
#include "ui/common/ingest_dialog.h"
#include "ui/common/theme.h"
#include "ui/evidence_browser/evidence_panel.h"
#include "ui/inspector/inspector_panel.h"
#include "ui/timeline/timeline_widget.h"
#include "ui/viewer/viewer_panel.h"

namespace trace {
namespace {

/// Where to drop screenshots when the harness asks for them.
std::filesystem::path screenshotDirectory() {
    if (const char* directory = std::getenv("TRACE_SCREENSHOT_DIR");
        directory != nullptr && *directory != '\0') {
        std::filesystem::create_directories(directory);
        return directory;
    }
    return {};
}

void captureScreenshot(QWidget* widget, const std::string& name) {
    const auto directory = screenshotDirectory();
    if (directory.empty() || widget == nullptr) return;
    QApplication::processEvents();
    const QPixmap pixmap = widget->grab();
    pixmap.save(QString::fromStdString((directory / (name + ".png")).string()));
}

/// Spins the event loop until `predicate` holds or the timeout expires, so the
/// test observes the same asynchronous behaviour a user would.
bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) return true;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

/// Registers a one-shot handler for the next modal dialog. Modal dialogs run
/// their own event loop, so the handler has to be armed before the action that
/// opens them.
class ModalResponder : public QObject {
public:
    using Handler = std::function<bool(QWidget*)>;

    explicit ModalResponder(Handler handler) : handler_(std::move(handler)) {
        timer_ = new QTimer(this);
        timer_->setInterval(25);
        connect(timer_, &QTimer::timeout, this, [this] {
            QWidget* modal = QApplication::activeModalWidget();
            if (modal == nullptr) return;
            if (handler_(modal)) {
                handled_ = true;
                timer_->stop();
            }
        });
        timer_->start();
    }

    bool handled() const { return handled_; }

private:
    QTimer* timer_ = nullptr;
    Handler handler_;
    bool handled_ = false;
};

/// Dismisses any message box that appears (the real UI shows one after an
/// integrity check) and remembers its text so the test can assert on it.
class MessageBoxCollector : public QObject {
public:
    MessageBoxCollector() {
        timer_ = new QTimer(this);
        timer_->setInterval(25);
        connect(timer_, &QTimer::timeout, this, [this] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (box == nullptr) return;
            lastText_ = box->text();
            box->accept();
        });
        timer_->start();
    }

    QString lastText() const { return lastText_; }
    void clear() { lastText_.clear(); }

private:
    QTimer* timer_ = nullptr;
    QString lastText_;
};

QAction* findAction(QWidget* widget, const QString& text) {
    for (QAction* action : widget->findChildren<QAction*>()) {
        if (action->text().contains(text, Qt::CaseInsensitive)) return action;
    }
    return nullptr;
}

constexpr std::int64_t kFrameDurationUs = 40'000;

TEST(AcceptancePhase0, TheDocumentedWorkflowSucceedsEndToEnd) {
    testing::TemporaryDirectory dataRoot("trace-acceptance");
    // Printed so a failed run can be inspected before the directory is removed.
    std::cerr << "TRACE data root: " << dataRoot.path() << std::endl;
    const auto incoming = dataRoot.path() / "incoming";
    std::filesystem::create_directories(incoming);
    const auto source = incoming / "sample.mp4";
    std::filesystem::copy_file(testing::sampleVideoPath(), source);

    auto independentDigest = hashFile(source);
    ASSERT_TRUE(independentDigest.ok());

    std::string caseId;
    std::string evidenceId;
    std::string recordedDigest;

    // ═══════════════════════════════ SESSION ONE ═══════════════════════════
    {
        // 1. Launch TRACE.
        ui::ApplicationContext context;
        ASSERT_TRUE(context.initialise(dataRoot.path()).ok());
        ui::MainWindow window(&context);
        window.resize(1600, 950);
        window.show();
        ASSERT_TRUE(waitFor([&] { return window.isVisible(); }, 10000));
        captureScreenshot(&window, "01-cases-empty");

        MessageBoxCollector messageBoxes;

        // 2. Create CASE-0001 through the File menu and its dialog.
        {
            ModalResponder responder([](QWidget* modal) {
                auto* dialog = qobject_cast<ui::CaseDialog*>(modal);
                if (dialog == nullptr) return false;
                auto* title = dialog->findChild<QLineEdit*>(QStringLiteral("titleField"));
                auto* investigator =
                    dialog->findChild<QLineEdit*>(QStringLiteral("investigatorField"));
                auto* number = dialog->findChild<QLineEdit*>(QStringLiteral("caseNumberField"));
                if (title == nullptr || number == nullptr) return false;
                EXPECT_EQ(number->text(), QStringLiteral("CASE-0001"))
                    << "the next case number should be offered automatically";
                title->setText(QStringLiteral("Sample Incident"));
                if (investigator != nullptr) investigator->setText(QStringLiteral("A. Analyst"));
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (buttons == nullptr) return false;
                buttons->button(QDialogButtonBox::Ok)->click();
                return true;
            });

            QAction* newCase = findAction(&window, QStringLiteral("New case"));
            ASSERT_NE(newCase, nullptr);
            newCase->trigger();
            ASSERT_TRUE(waitFor([&] { return responder.handled(); }, 10000));
        }

        ASSERT_TRUE(waitFor([&] { return context.currentCase().has_value(); }, 10000));
        ASSERT_TRUE(context.currentCase().has_value());
        EXPECT_EQ(context.currentCase()->caseNumber, "CASE-0001");
        EXPECT_EQ(context.currentCase()->title, "Sample Incident");
        caseId = context.currentCase()->id;
        captureScreenshot(&window, "02-case-open");

        // 3. Import sample.mp4 through the real ingest dialog.
        {
            ui::IngestDialog dialog(&context,
                                    QStringList{QString::fromStdString(source.string())}, &window);
            dialog.show();
            ASSERT_TRUE(waitFor([&] { return dialog.finished(); }, 60000))
                << "the import did not complete";
            captureScreenshot(&dialog, "03-import-complete");
            EXPECT_EQ(dialog.importedCount(), 1) << dialog.failures().join(QStringLiteral("; ")).toStdString();
            dialog.close();
        }
        window.evidencePanel()->refresh();
        QApplication::processEvents();

        auto evidenceList = context.evidence().listForCase(caseId);
        ASSERT_TRUE(evidenceList.ok());
        ASSERT_EQ(evidenceList.value().size(), 1u);
        const Evidence evidence = evidenceList.value().front();
        evidenceId = evidence.id;
        recordedDigest = evidence.sha256;

        // TRACE must have copied the original, preserved its bytes and hashed it.
        EXPECT_EQ(evidence.evidenceNumber, "EVD-000001");
        EXPECT_EQ(recordedDigest, independentDigest.value())
            << "the recorded digest does not match an independently computed SHA-256";
        const auto managed = context.layout().resolve(evidence.storageRelPath);
        EXPECT_TRUE(std::filesystem::exists(managed));
        EXPECT_EQ(std::filesystem::file_size(managed), std::filesystem::file_size(source));
        EXPECT_TRUE(std::filesystem::exists(source)) << "the source file must be untouched";

        // 6. The evidence appears in the case.
        window.evidencePanel()->selectEvidence(QString::fromStdString(evidenceId));
        ASSERT_TRUE(waitFor([&] { return context.currentEvidence().has_value(); }, 10000));

        // 7. Play the video.
        ui::ViewerPanel* viewer = window.viewerPanel();
        ASSERT_TRUE(waitFor([&] { return viewer->playback()->isOpen(); }, 20000))
            << "the viewer never opened the media";
        ASSERT_TRUE(waitFor([&] { return viewer->duration() > 0; }, 10000));
        EXPECT_NEAR(static_cast<double>(viewer->duration()), 8'000'000.0, 200'000.0);
        captureScreenshot(&window, "04-viewer-ready");

        viewer->playback()->setSpeed(4.0);
        viewer->togglePlayPause();
        ASSERT_TRUE(waitFor([&] { return viewer->position() > 500'000; }, 20000))
            << "playback did not advance";
        viewer->playback()->pause();
        ASSERT_TRUE(waitFor(
            [&] { return viewer->playback()->state() == PlaybackState::Paused; }, 10000));
        captureScreenshot(&window, "05-playing");

        // 8. Seek accurately.
        viewer->seek(3'000'000);
        ASSERT_TRUE(waitFor([&] {
            return viewer->position() <= 3'000'000 &&
                   viewer->position() > 3'000'000 - kFrameDurationUs;
        }, 15000)) << "seek did not land on the requested frame, position was "
                   << viewer->position();

        // 9. Step by frame, forward and back.
        const qint64 beforeStep = viewer->position();
        viewer->stepForward();
        ASSERT_TRUE(waitFor([&] { return viewer->position() == beforeStep + kFrameDurationUs; },
                            15000))
            << "forward frame step landed at " << viewer->position();
        viewer->stepBackward();
        ASSERT_TRUE(waitFor([&] { return viewer->position() == beforeStep; }, 15000))
            << "backward frame step landed at " << viewer->position();

        // 10. Create the bookmark at 00:00:05.
        viewer->seek(5'000'000);
        ASSERT_TRUE(waitFor([&] {
            return viewer->position() <= 5'000'000 &&
                   viewer->position() > 5'000'000 - kFrameDurationUs;
        }, 15000));

        {
            ModalResponder responder([](QWidget* modal) {
                auto* dialog = qobject_cast<ui::BookmarkDialog*>(modal);
                if (dialog == nullptr) return false;
                auto* timecode = dialog->findChild<QLineEdit*>(QStringLiteral("timecodeField"));
                auto* title = dialog->findChild<QLineEdit*>(QStringLiteral("titleField"));
                if (timecode == nullptr || title == nullptr) return false;
                timecode->setText(QStringLiteral("00:00:05.000"));
                title->setText(QStringLiteral("Test Bookmark"));
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (buttons == nullptr) return false;
                buttons->button(QDialogButtonBox::Ok)->click();
                return true;
            });
            window.addBookmarkAtPlayhead();
            ASSERT_TRUE(waitFor([&] { return responder.handled(); }, 10000));
        }

        auto bookmarks = context.annotations().bookmarksForEvidence(evidenceId);
        ASSERT_TRUE(bookmarks.ok());
        ASSERT_EQ(bookmarks.value().size(), 1u) << "the bookmark was not created";
        EXPECT_EQ(bookmarks.value().front().timestampUs, 5'000'000);
        EXPECT_EQ(bookmarks.value().front().title, "Test Bookmark");

        // The timeline shows it.
        EXPECT_GT(window.timeline()->duration(), 0);
        EXPECT_EQ(window.bookmarksPanel()->bookmarks().size(), 1u);
        captureScreenshot(&window, "06-bookmark-created");

        // Saving a frame produces a derived asset without touching the original.
        window.saveCurrentFrame();
        ASSERT_TRUE(waitFor([&] {
            auto assets = context.derivedAssets().listForEvidence(evidenceId);
            if (!assets) return false;
            return std::any_of(assets.value().begin(), assets.value().end(),
                               [](const DerivedAsset& asset) {
                                   return asset.type == DerivedAssetType::FrameExport;
                               });
        }, 30000)) << "the exported frame was not registered as a derived asset";

        // 11. Close TRACE.
        window.close();
        QApplication::processEvents();
    }

    // ═══════════════════════════════ SESSION TWO ═══════════════════════════
    {
        // 12. Reopen TRACE against the same data directory.
        ui::ApplicationContext context;
        ASSERT_TRUE(context.initialise(dataRoot.path()).ok());
        ui::MainWindow window(&context);
        window.resize(1600, 950);
        window.show();
        ASSERT_TRUE(waitFor([&] { return window.isVisible(); }, 10000));
        MessageBoxCollector messageBoxes;

        // 13. Reopen CASE-0001.
        auto reopened = context.cases().findByCaseNumber("CASE-0001");
        ASSERT_TRUE(reopened.ok());
        ASSERT_TRUE(reopened.value().has_value()) << "CASE-0001 did not survive the restart";
        ASSERT_TRUE(context.openCase(QString::fromStdString(reopened.value()->id)).ok());
        window.showViewer();
        QApplication::processEvents();

        // 14. The imported video is still there.
        auto evidenceList = context.evidence().listForCase(caseId);
        ASSERT_TRUE(evidenceList.ok());
        ASSERT_EQ(evidenceList.value().size(), 1u);
        const Evidence evidence = evidenceList.value().front();
        EXPECT_EQ(evidence.id, evidenceId);
        EXPECT_EQ(evidence.sha256, recordedDigest);

        window.evidencePanel()->refresh();
        window.evidencePanel()->selectEvidence(QString::fromStdString(evidenceId));
        ASSERT_TRUE(waitFor([&] { return context.currentEvidence().has_value(); }, 10000));

        // 15. The bookmark is still there.
        auto bookmarks = context.annotations().bookmarksForEvidence(evidenceId);
        ASSERT_TRUE(bookmarks.ok());
        ASSERT_EQ(bookmarks.value().size(), 1u) << "the bookmark did not survive the restart";
        EXPECT_EQ(bookmarks.value().front().title, "Test Bookmark");
        EXPECT_EQ(bookmarks.value().front().timestampUs, 5'000'000);
        captureScreenshot(&window, "07-reopened");

        // 16./17. Verify integrity through the inspector, exactly as an
        // operator would, and confirm the digest still matches.
        window.inspectorPanel()->verifyIntegrity();
        ASSERT_TRUE(waitFor([&] {
            auto reloaded = context.evidence().findById(evidenceId);
            return reloaded.ok() && reloaded.value().has_value() &&
                   reloaded.value()->integrityStatus == IntegrityStatus::Verified &&
                   reloaded.value()->lastVerifiedAt.has_value();
        }, 60000)) << "integrity verification did not complete";

        // The database is updated by the worker before the result dialog is
        // shown, so wait for the dialog itself before reading its text.
        ASSERT_TRUE(waitFor([&] { return !messageBoxes.lastText().isEmpty(); }, 30000))
            << "no verification result was shown to the operator";
        EXPECT_TRUE(messageBoxes.lastText().contains(QStringLiteral("VERIFIED")))
            << messageBoxes.lastText().toStdString();
        EXPECT_TRUE(messageBoxes.lastText().contains(QString::fromStdString(recordedDigest)))
            << "the result should show the digest that was compared";

        auto verified = context.evidence().findById(evidenceId);
        ASSERT_TRUE(verified.ok());
        EXPECT_EQ(verified.value()->sha256, recordedDigest)
            << "verification must not rewrite the recorded digest";
        captureScreenshot(&window, "08-integrity-verified");

        // 18./19. The audit trail contains the whole story.
        window.showAudit();
        window.auditPanel()->refresh();
        QApplication::processEvents();
        captureScreenshot(&window, "09-audit-trail");

        AuditQuery query;
        query.caseId = caseId;
        query.newestFirst = false;
        query.limit = 0;
        auto events = context.audit().list(query);
        ASSERT_TRUE(events.ok());

        const auto has = [&](AuditAction action) {
            return std::any_of(events.value().begin(), events.value().end(),
                               [&](const AuditEvent& event) { return event.action == action; });
        };
        EXPECT_TRUE(has(AuditAction::CaseCreated));
        EXPECT_TRUE(has(AuditAction::CaseOpened));
        EXPECT_TRUE(has(AuditAction::EvidenceImported));
        EXPECT_TRUE(has(AuditAction::EvidenceHashed));
        EXPECT_TRUE(has(AuditAction::EvidenceMetadataExtracted));
        EXPECT_TRUE(has(AuditAction::EvidenceViewed));
        EXPECT_TRUE(has(AuditAction::BookmarkCreated));
        EXPECT_TRUE(has(AuditAction::FrameExtracted));
        EXPECT_TRUE(has(AuditAction::IntegrityVerified));

        // Application start/stop is recorded across both sessions.
        AuditQuery applicationQuery;
        applicationQuery.action = AuditAction::ApplicationStarted;
        auto starts = context.audit().count(applicationQuery);
        ASSERT_TRUE(starts.ok());
        EXPECT_GE(starts.value(), 2);

        window.close();
        QApplication::processEvents();
    }
}

}  // namespace
}  // namespace trace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    // Same theme the shipped application applies, so screenshots taken here
    // show what an operator actually sees.
    trace::ui::applyTraceTheme(application);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
