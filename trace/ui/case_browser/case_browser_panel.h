#pragma once

#include <QWidget>

#include <vector>

#include "core/models/case.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace trace::ui {

class ApplicationContext;

/// The case list: search, filter, create, open.
class CaseBrowserPanel : public QWidget {
    Q_OBJECT

public:
    explicit CaseBrowserPanel(ApplicationContext* context, QWidget* parent = nullptr);

    void refresh();

signals:
    void caseOpened();

private:
    void createCase();
    void openSelected();
    void editSelected();
    std::optional<Case> selectedCase() const;

    ApplicationContext* context_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* statusFilter_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* openButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    std::vector<Case> cases_;
};

}  // namespace trace::ui
