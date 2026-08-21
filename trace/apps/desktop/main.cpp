// TRACE desktop entry point.
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>

#include <cstdlib>
#include <filesystem>

#include "core/common/logging.h"
#include "core/storage/storage_layout.h"
#include "trace/trace_version.h"
#include "ui/app/application_context.h"
#include "ui/app/main_window.h"
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

    trace::ui::MainWindow window(&context);
    window.show();

    const int result = application.exec();
    context.shutdown();
    return result;
}
