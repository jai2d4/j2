// TRACE desktop entry point.
#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QMessageBox>

#include <cstdlib>
#include <filesystem>

#include "core/common/logging.h"
#include "core/storage/storage_layout.h"
#include "trace/trace_version.h"
#include "ui/app/application_context.h"
#include "ui/app/main_window.h"
#include "ui/auth/sign_in_dialog.h"
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
    parser.process(application);

    trace::ui::applyTraceTheme(application);

    std::filesystem::path dataRoot = trace::StorageLayout::defaultDataRoot();
    if (parser.isSet(dataDirectoryOption)) {
        dataRoot = parser.value(dataDirectoryOption).toStdString();
    }

    trace::ui::ApplicationContext context;
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
