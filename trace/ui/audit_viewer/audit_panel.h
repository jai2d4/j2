#pragma once

#include <QWidget>

#include <vector>

#include "core/models/audit_event.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;

namespace trace::ui {

class ApplicationContext;

/// The audit trail viewer.
///
/// Read-only by construction: the repository exposes no update or delete, and
/// the database refuses both through triggers. This panel shows records and
/// their structured detail, and nothing here can change them.
class AuditPanel : public QWidget {
    Q_OBJECT

public:
    explicit AuditPanel(ApplicationContext* context, QWidget* parent = nullptr);

    void refresh();

private:
    void showDetails(int row);

    ApplicationContext* context_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* actionFilter_ = nullptr;
    QCheckBox* currentCaseOnly_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QLabel* summary_ = nullptr;
    std::vector<AuditEvent> events_;
};

}  // namespace trace::ui
