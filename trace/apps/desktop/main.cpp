// TRACE desktop entry point.
#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QLineEdit>
#include <QProgressDialog>
#include <QMessageBox>

#include <cstdlib>
#include <filesystem>

#include "core/common/logging.h"
#include "core/services/workspace_service.h"
#include "core/storage/storage_layout.h"
#include "trace/trace_version.h"
#include "ui/app/application_context.h"
#include "ui/app/main_window.h"
#include "ui/auth/sign_in_dialog.h"
#include "ui/auth/unlock_dialog.h"
#include "ui/common/theme.h"

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QString::fromUtf8(trace::kApplicationName));
    QApplication::setApplicationVersion(QString::fromUtf8(trace::kApplicationVersion));
    QApplication::setOrganizationName(QString::fromUtf8(trace::kOrganizationName));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("TRACE — video evidence intelligence platform (Phase 0)"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption dataDirectoryOption(
        {QStringLiteral("d"), QStringLiteral("data-dir")},
        QStringLiteral("Directory holding the case database and managed evidence storage."),
        QStringLiteral("path"));
    parser.addOption(dataDirectoryOption);
    QCommandLineOption encryptOption(
        QStringLiteral("encrypt-workspace"),
        QStringLiteral("Convert an existing data directory to encrypted storage, then exit."));
    parser.addOption(encryptOption);
    parser.process(application);

    trace::ui::applyTraceTheme(application);

    std::filesystem::path dataRoot = trace::StorageLayout::defaultDataRoot();
    if (parser.isSet(dataDirectoryOption)) {
        dataRoot = parser.value(dataDirectoryOption).toStdString();
    }

    // The key comes before the database, and has to: the users table that a
    // sign-in reads lives inside the encrypted file. So an encrypted workspace
    // is unlocked first, from the keyring alone, and only then is anything
    // opened.
    const trace::WorkspaceState workspace = trace::ui::ApplicationContext::inspectWorkspace(dataRoot);

    // Conversion runs with nothing else holding the workspace open, which is why
    // it is a mode of its own rather than a menu item that does the work: every
    // managed original is rewritten and the database is re-keyed, and doing that
    // underneath an open case is how a half-converted workspace happens.
    if (parser.isSet(encryptOption)) {
        trace::ui::UnlockWorkspaceDialog setup(
            dataRoot, workspace,
            workspace.encrypted ? trace::ui::UnlockWorkspaceDialog::Mode::Unlock
                                : trace::ui::UnlockWorkspaceDialog::Mode::SetUp);
        if (setup.exec() != QDialog::Accepted) return 0;

        QProgressDialog progress(QStringLiteral("Encrypting evidence…"), QStringLiteral("Cancel"),
                                 0, 100);
        progress.setWindowTitle(QStringLiteral("TRACE — encrypting workspace"));
        progress.setWindowModality(Qt::ApplicationModal);
        progress.setMinimumDuration(0);
        progress.setValue(0);

        const auto report = [&progress](const trace::ConversionProgress& state) {
            if (state.filesTotal > 0) {
                progress.setMaximum(static_cast<int>(state.filesTotal));
                progress.setValue(static_cast<int>(state.filesDone));
            }
            progress.setLabelText(
                QStringLiteral("Encrypting %1 (%2 of %3)…")
                    .arg(QString::fromStdString(state.currentEvidenceNumber))
                    .arg(state.filesDone + 1)
                    .arg(state.filesTotal));
            QApplication::processEvents();
            return !progress.wasCanceled();
        };

        auto converted = trace::WorkspaceService::encryptExistingWorkspace(
            dataRoot, setup.username().toStdString(), setup.password().toStdString(), report);
        progress.close();

        if (!converted) {
            if (converted.error().code() == trace::ErrorCode::Cancelled) {
                QMessageBox::information(
                    nullptr, QStringLiteral("Encryption stopped"),
                    QStringLiteral(
                        "The conversion was cancelled. Files already converted stay encrypted and "
                        "the rest are untouched — both are readable, and running this again "
                        "finishes the job."));
                return 0;
            }
            QMessageBox::critical(
                nullptr, QStringLiteral("Workspace not encrypted"),
                QStringLiteral("%1\n\nNothing was lost: the file that failed was left exactly as "
                               "it was, and anything already converted is still readable.")
                    .arg(QString::fromStdString(converted.error().toString())));
            return 1;
        }
        QMessageBox::information(
            nullptr, QStringLiteral("Workspace encrypted"),
            QStringLiteral("The case database and every recording in this workspace are now "
                           "stored encrypted. The previous database was kept alongside it as "
                           "trace.db.plain — delete it once you are satisfied, since it still "
                           "holds the case index in the clear."));
        return 0;
    }

    trace::ui::ApplicationContext context;

    QString suggestedUser;
    if (workspace.encrypted || !workspace.exists) {
        const auto mode = workspace.encrypted ? trace::ui::UnlockWorkspaceDialog::Mode::Unlock
                                              : trace::ui::UnlockWorkspaceDialog::Mode::SetUp;
        trace::ui::UnlockWorkspaceDialog unlock(dataRoot, workspace, mode);
        if (unlock.exec() != QDialog::Accepted) return 0;

        if (workspace.encrypted || unlock.encryptionChosen()) {
            if (auto status = unlock.applyTo(context.keys()); !status) {
                QMessageBox::critical(
                    nullptr, QStringLiteral("TRACE could not start"),
                    QStringLiteral("The workspace could not be unlocked.\n\n%1")
                        .arg(QString::fromStdString(status.error().toString())));
                return 1;
            }
            // Offered to the sign-in that follows, so an operator who has just
            // set up a workspace is not asked for the same name twice.
            suggestedUser = unlock.username();
        }
    }

    if (auto status = context.initialise(dataRoot); !status) {
        // A failure here means no database and therefore no chain of custody:
        // say exactly what happened rather than opening a window that cannot work.
        trace::logCritical("startup", "TRACE could not start",
                           trace::JsonValue::object()
                               .set("detail", status.error().toString())
                               .set("data_root", dataRoot.string()));
        QMessageBox::critical(
            nullptr, QStringLiteral("TRACE could not start"),
            QStringLiteral("The evidence database could not be opened.\n\n%1\n\nData directory: %2")
                .arg(QString::fromStdString(status.error().toString()),
                     QString::fromStdString(dataRoot.string())));
        return 1;
    }

    // Nothing opens until somebody has said who they are. TRACE's purpose is
    // recording who did what to a piece of evidence, so an unattributed session
    // is not a lesser version of the product — it is the wrong product.
    {
        trace::ui::SignInDialog gate(&context, trace::ui::SignInDialog::modeFor(&context));
        if (!suggestedUser.isEmpty()) gate.usernameField()->setText(suggestedUser);
        if (gate.exec() != QDialog::Accepted) {
            context.shutdown();
            return 0;
        }
    }

    // An account still holding a password an administrator issued replaces it
    // before going any further: a credential somebody else knows cannot be the
    // one another person's audited actions are attributed to.
    while (context.auth().mustChangePassword()) {
        trace::ui::SignInDialog change(&context,
                                       trace::ui::SignInDialog::Mode::ChangePassword);
        if (change.exec() != QDialog::Accepted) {
            context.auth().signOut();
            context.shutdown();
            return 0;
        }
    }

    trace::ui::MainWindow window(&context);
    window.show();

    const int result = application.exec();
    context.shutdown();
    return result;
}
