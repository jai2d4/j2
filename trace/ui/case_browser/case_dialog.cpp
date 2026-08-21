#include "ui/case_browser/case_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include "ui/app/application_context.h"
#include "ui/common/theme.h"

namespace trace::ui {
namespace {

const std::vector<CaseStatus> kStatuses = {CaseStatus::Open, CaseStatus::Active, CaseStatus::OnHold,
                                           CaseStatus::Closed, CaseStatus::Archived};

}  // namespace

CaseDialog::CaseDialog(ApplicationContext* context, Mode mode, std::optional<Case> existing,
                       QWidget* parent)
    : QDialog(parent), context_(context), mode_(mode), existing_(std::move(existing)) {
    setWindowTitle(mode == Mode::Create ? QStringLiteral("New case") : QStringLiteral("Edit case"));
    setModal(true);
    setMinimumWidth(520);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(14);

    auto* heading = new QLabel(mode == Mode::Create ? QStringLiteral("NEW CASE")
                                                    : QStringLiteral("EDIT CASE"),
                               this);
    heading->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                          "letter-spacing: 1.5px;")
                               .arg(colors::kTextSecondary.name()));
    layout->addWidget(heading);

    auto* form = new QFormLayout();
    form->setSpacing(10);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    caseNumber_ = new QLineEdit(this);
    caseNumber_->setObjectName(QStringLiteral("caseNumberField"));
    title_ = new QLineEdit(this);
    title_->setObjectName(QStringLiteral("titleField"));
    description_ = new QPlainTextEdit(this);
    description_->setObjectName(QStringLiteral("descriptionField"));
    description_->setFixedHeight(64);
    status_ = new QComboBox(this);
    status_->setObjectName(QStringLiteral("statusCombo"));
    for (const auto value : kStatuses) {
        status_->addItem(QString::fromUtf8(toDisplayString(value)), static_cast<int>(value));
    }
    investigator_ = new QLineEdit(this);
    investigator_->setObjectName(QStringLiteral("investigatorField"));
    agency_ = new QLineEdit(this);
    agency_->setObjectName(QStringLiteral("agencyField"));
    location_ = new QLineEdit(this);
    location_->setObjectName(QStringLiteral("locationField"));
    notes_ = new QPlainTextEdit(this);
    notes_->setObjectName(QStringLiteral("notesField"));
    notes_->setFixedHeight(64);

    form->addRow(QStringLiteral("Case number"), caseNumber_);
    form->addRow(QStringLiteral("Title"), title_);
    form->addRow(QStringLiteral("Description"), description_);
    form->addRow(QStringLiteral("Status"), status_);
    form->addRow(QStringLiteral("Investigator / analyst"), investigator_);
    form->addRow(QStringLiteral("Agency / client"), agency_);
    form->addRow(QStringLiteral("Location"), location_);
    form->addRow(QStringLiteral("Notes"), notes_);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)
        ->setText(mode == Mode::Create ? QStringLiteral("Create case") : QStringLiteral("Save"));
    styleAsPrimaryAction(buttons->button(QDialogButtonBox::Ok));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &CaseDialog::submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (mode_ == Mode::Create) {
        auto suggested = context_->cases().suggestNextCaseNumber();
        if (suggested) caseNumber_->setText(QString::fromStdString(suggested.value()));
        title_->setFocus();
    } else if (existing_) {
        caseNumber_->setText(QString::fromStdString(existing_->caseNumber));
        title_->setText(QString::fromStdString(existing_->title));
        description_->setPlainText(QString::fromStdString(existing_->description));
        investigator_->setText(QString::fromStdString(existing_->investigator));
        agency_->setText(QString::fromStdString(existing_->agency));
        location_->setText(QString::fromStdString(existing_->location));
        notes_->setPlainText(QString::fromStdString(existing_->notes));
        const int index = status_->findData(static_cast<int>(existing_->status));
        if (index >= 0) status_->setCurrentIndex(index);
    }
}

void CaseDialog::submit() {
    if (title_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Title required"),
                             QStringLiteral("A case must have a title."));
        title_->setFocus();
        return;
    }

    const auto status = static_cast<CaseStatus>(status_->currentData().toInt());

    if (mode_ == Mode::Create) {
        CaseDraft draft;
        draft.caseNumber = caseNumber_->text().trimmed().toStdString();
        draft.title = title_->text().trimmed().toStdString();
        draft.description = description_->toPlainText().toStdString();
        draft.status = status;
        draft.investigator = investigator_->text().trimmed().toStdString();
        draft.agency = agency_->text().trimmed().toStdString();
        draft.location = location_->text().trimmed().toStdString();
        draft.notes = notes_->toPlainText().toStdString();

        auto created = context_->cases().createCase(draft);
        if (!created) {
            QMessageBox::critical(this, QStringLiteral("Case not created"),
                                  QString::fromStdString(created.error().toString()));
            return;
        }
        result_ = created.take();
    } else {
        Case updated = *existing_;
        updated.caseNumber = caseNumber_->text().trimmed().toStdString();
        updated.title = title_->text().trimmed().toStdString();
        updated.description = description_->toPlainText().toStdString();
        updated.status = status;
        updated.investigator = investigator_->text().trimmed().toStdString();
        updated.agency = agency_->text().trimmed().toStdString();
        updated.location = location_->text().trimmed().toStdString();
        updated.notes = notes_->toPlainText().toStdString();

        if (auto saved = context_->cases().updateCase(updated); !saved) {
            QMessageBox::critical(this, QStringLiteral("Case not saved"),
                                  QString::fromStdString(saved.error().toString()));
            return;
        }
        result_ = updated;
    }
    accept();
}

}  // namespace trace::ui
