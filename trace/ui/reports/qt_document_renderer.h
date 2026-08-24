#pragma once

#include "reporting/report_service.h"

namespace trace::ui {

/// Renders the report to PDF with Qt's text engine.
///
/// This is the whole reason the seam exists: `reporting/` has no Qt, and laying out a
/// document for print needs a font and layout engine. The bundle's HTML remains the
/// source of truth — the PDF is a paginated view of exactly that markup, produced from
/// the same string that was written to REPORT.html.
class QtDocumentRenderer : public IDocumentRenderer {
public:
    Status renderPdf(const std::string& html, const std::filesystem::path& destination) override;
};

}  // namespace trace::ui
