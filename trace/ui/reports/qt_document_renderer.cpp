#include "ui/reports/qt_document_renderer.h"

#include <QGuiApplication>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QTextDocument>

#include <filesystem>
#include <system_error>

namespace trace::ui {

Status QtDocumentRenderer::renderPdf(const std::string& html,
                                     const std::filesystem::path& destination) {
    if (QGuiApplication::instance() == nullptr) {
        return Status::failure(ErrorCode::Unsupported,
                               "A paginated report needs a running GUI application");
    }

    QTextDocument document;
    document.setHtml(QString::fromStdString(html));

    QPdfWriter writer(QString::fromStdString(destination.string()));
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(14, 14, 14, 14), QPageLayout::Millimeter);
    writer.setResolution(300);
    writer.setTitle(document.metaInformation(QTextDocument::DocumentTitle));

    document.setPageSize(QSizeF(writer.pageLayout().paintRectPixels(writer.resolution()).size()));
    document.print(&writer);

    // QPdfWriter reports nothing, so the only honest check is whether a file with
    // content actually appeared.
    std::error_code ec;
    if (!std::filesystem::exists(destination, ec) ||
        std::filesystem::file_size(destination, ec) == 0) {
        return Status::failure(ErrorCode::IoError, "The PDF was not written");
    }
    return Status::success();
}

}  // namespace trace::ui
