#include "ui/case_browser/case_browser_panel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "ui/app/application_context.h"
#include "ui/case_browser/case_dialog.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"

namespace trace::ui {

CaseBrowserPanel::CaseBrowserPanel(ApplicationContext* context, QWidget* parent)
    : QWidget(parent), context_(context) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("CASES"), this);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 700; "
                                        "letter-spacing: 2px;")
                             .arg(colors::kTextSecondary.name()));
    header->addWidget(title);
    header->addStretch();

    auto* newCase = new QPushButton(QStringLiteral("New case"), this);
    styleAsPrimaryAction(newCase);
    header->addWidget(newCase);
    layout->addLayout(header);

    auto* filters = new QHBoxLayout();
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(
        QStringLiteral("Search case number, title, investigator, agency or location"));
    search_->setClearButtonEnabled(true);
    filters->addWidget(search_, 1);

    statusFilter_ = new QComboBox(this);
    statusFilter_->addItem(QStringLiteral("All statuses"), -1);
    for (const auto status : {CaseStatus::Open, CaseStatus::Active, CaseStatus::OnHold,
                              CaseStatus::Closed, CaseStatus::Archived}) {
        statusFilter_->addItem(QString::fromUtf8(toDisplayString(status)), static_cast<int>(status));
    }
    filters->addWidget(statusFilter_);
    layout->addLayout(filters);

    table_ = new QTableWidget(this);
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels({QStringLiteral("Case number"), QStringLiteral("Title"),
                                       QStringLiteral("Status"), QStringLiteral("Investigator"),
                                       QStringLiteral("Evidence"), QStringLiteral("Last modified")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->setSortingEnabled(false);
    layout->addWidget(table_, 1);

    auto* footer = new QHBoxLayout();
    summary_ = new QLabel(this);
    summary_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;").arg(colors::kTextSecondary.name()));
    footer->addWidget(summary_);
    footer->addStretch();
    editButton_ = new QPushButton(QStringLiteral("Edit case"), this);
    openButton_ = new QPushButton(QStringLiteral("Open case"), this);
    styleAsPrimaryAction(openButton_);
    editButton_->setEnabled(false);
    openButton_->setEnabled(false);
    footer->addWidget(editButton_);
    footer->addWidget(openButton_);
    layout->addLayout(footer);

    connect(newCase, &QPushButton::clicked, this, &CaseBrowserPanel::createCase);
    connect(openButton_, &QPushButton::clicked, this, &CaseBrowserPanel::openSelected);
    connect(editButton_, &QPushButton::clicked, this, &CaseBrowserPanel::editSelected);
    connect(table_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem*) { openSelected(); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        const bool hasSelection = !table_->selectedItems().isEmpty();
        openButton_->setEnabled(hasSelection);
        editButton_->setEnabled(hasSelection);
    });
    connect(search_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(statusFilter_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(context_, &ApplicationContext::casesChanged, this, &CaseBrowserPanel::refresh);

    refresh();
}

void CaseBrowserPanel::refresh() {
    CaseQuery query;
    query.text = search_->text().trimmed().toStdString();
    if (const int status = statusFilter_->currentData().toInt(); status >= 0) {
        query.status = static_cast<CaseStatus>(status);
    }

    auto listed = context_->cases().listCases(query);
    if (!listed) {
        summary_->setText(QStringLiteral("Unable to read cases: %1")
                              .arg(QString::fromStdString(listed.error().toString())));
        return;
    }
    cases_ = listed.take();

    table_->setRowCount(static_cast<int>(cases_.size()));
    for (int row = 0; row < static_cast<int>(cases_.size()); ++row) {
        const Case& value = cases_[static_cast<std::size_t>(row)];
        auto* numberItem = new QTableWidgetItem(QString::fromStdString(value.caseNumber));
        numberItem->setFont(monospaceFont());
        table_->setItem(row, 0, numberItem);
        table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(value.title)));

        auto* statusItem = new QTableWidgetItem(QString::fromUtf8(toDisplayString(value.status)));
        statusItem->setForeground(value.status == CaseStatus::Closed ? colors::kTextSecondary
                                                                     : colors::kAccent);
        table_->setItem(row, 2, statusItem);
        table_->setItem(row, 3,
                        new QTableWidgetItem(value.investigator.empty()
                                                 ? kNotAvailable
                                                 : QString::fromStdString(value.investigator)));

        auto count = context_->evidence().countForCase(value.id);
        table_->setItem(row, 4,
                        new QTableWidgetItem(count ? QString::number(count.value())
                                                   : QStringLiteral("?")));
        table_->setItem(row, 5, new QTableWidgetItem(timestamp(value.modifiedAt)));
    }
    table_->resizeColumnsToContents();
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    summary_->setText(cases_.empty()
                          ? QStringLiteral("No cases yet — create one to begin.")
                          : QStringLiteral("%1 case(s)").arg(cases_.size()));
}

std::optional<Case> CaseBrowserPanel::selectedCase() const {
    const int row = table_->currentRow();
    if (row < 0 || row >= static_cast<int>(cases_.size())) return std::nullopt;
    return cases_[static_cast<std::size_t>(row)];
}

void CaseBrowserPanel::createCase() {
    CaseDialog dialog(context_, CaseDialog::Mode::Create, std::nullopt, this);
    if (dialog.exec() != QDialog::Accepted || !dialog.result()) return;

    refresh();
    context_->notifyAuditChanged();
    if (auto status = context_->openCase(QString::fromStdString(dialog.result()->id)); !status) {
        QMessageBox::critical(this, QStringLiteral("Unable to open case"),
                              QString::fromStdString(status.error().toString()));
        return;
    }
    emit caseOpened();
}

void CaseBrowserPanel::openSelected() {
    const auto selected = selectedCase();
    if (!selected) return;
    if (auto status = context_->openCase(QString::fromStdString(selected->id)); !status) {
        QMessageBox::critical(this, QStringLiteral("Unable to open case"),
                              QString::fromStdString(status.error().toString()));
        return;
    }
    emit caseOpened();
}

void CaseBrowserPanel::editSelected() {
    const auto selected = selectedCase();
    if (!selected) return;
    CaseDialog dialog(context_, CaseDialog::Mode::Edit, selected, this);
    if (dialog.exec() != QDialog::Accepted) return;
    refresh();
    if (context_->currentCase() && context_->currentCase()->id == selected->id) {
        context_->refreshCurrentCase();
    }
    context_->notifyAuditChanged();
}

}  // namespace trace::ui
