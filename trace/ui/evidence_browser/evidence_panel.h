#pragma once

#include <QWidget>

#include <vector>

#include "core/models/evidence.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace trace::ui {

class ApplicationContext;

/// Evidence inside the open case: import, search, select.
///
/// Each row shows the evidence number, the original filename and two separate
/// states — integrity (does the stored file still match its digest) and media
/// (can TRACE decode it). They are never conflated.
class EvidencePanel : public QWidget {
    Q_OBJECT

public:
    explicit EvidencePanel(ApplicationContext* context, QWidget* parent = nullptr);

    void refresh();
    void importEvidence();
    void selectEvidence(const QString& evidenceId);

signals:
    void evidenceActivated(const Evidence& evidence);

private:
    void onSelectionChanged();

    ApplicationContext* context_ = nullptr;
    QLineEdit* search_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* importButton_ = nullptr;
    std::vector<Evidence> evidence_;
    bool updating_ = false;
};

}  // namespace trace::ui
