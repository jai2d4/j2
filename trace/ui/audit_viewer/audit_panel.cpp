#include "ui/audit_viewer/audit_panel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include "ui/app/application_context.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"

namespace trace::ui {
namespace {

const std::vector<AuditAction> kFilterableActions = {
    AuditAction::CaseCreated,       AuditAction::CaseOpened,
    AuditAction::CaseUpdated,       AuditAction::EvidenceImported,
    AuditAction::EvidenceImportFailed, AuditAction::EvidenceHashed,
    AuditAction::EvidenceMetadataExtracted, AuditAction::EvidenceViewed,
    AuditAction::IntegrityVerified, AuditAction::IntegrityFailed,
    AuditAction::BookmarkCreated,   AuditAction::BookmarkUpdated,
    AuditAction::BookmarkDeleted,   AuditAction::AnnotationCreated,
    AuditAction::AnnotationUpdated, AuditAction::AnnotationDeleted,
    AuditAction::FrameExtracted,    AuditAction::DerivedAssetCreated,
    AuditAction::SettingsChanged,   AuditAction::ApplicationStarted,
    AuditAction::ApplicationStopped, AuditAction::DatabaseMigrated};

QColor outcomeColour(const std::string& outcome) {
    if (outcome == "failure") return colors::kFailure;
    if (outcome == "warning") return colors::kWarning;
    return colors::kTextPrimary;
}

}  // namespace

AuditPanel::AuditPanel(ApplicationContext* context, QWidget* parent)
    : QWidget(parent), context_(context) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("AUDIT TRAIL"), this);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 700; "
                                        "letter-spacing: 2px;")
                             .arg(colors::kTextSecondary.name()));
    layout->addWidget(title);

    auto* caption =
        new QLabel(QStringLiteral("Append-only record of what TRACE and its operators did. Records "
                                  "cannot be edited or deleted from this application."),
                   this);
    caption->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    layout->addWidget(caption);

    auto* filters = new QHBoxLayout();
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Search description, actor or action"));
    search_->setClearButtonEnabled(true);
    filters->addWidget(search_, 1);

    actionFilter_ = new QComboBox(this);
    actionFilter_->addItem(QStringLiteral("All actions"), QString());
    for (const auto action : kFilterableActions) {
        actionFilter_->addItem(QString::fromUtf8(toDisplayString(action)),
                               QString::fromUtf8(toString(action)));
    }
    filters->addWidget(actionFilter_);

    currentCaseOnly_ = new QCheckBox(QStringLiteral("Current case only"), this);
    currentCaseOnly_->setChecked(true);
    filters->addWidget(currentCaseOnly_);
    layout->addLayout(filters);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    table_ = new QTableWidget(splitter);
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Timestamp (UTC)"),
                                       QStringLiteral("Action"), QStringLiteral("Case"),
                                       QStringLiteral("Evidence"), QStringLiteral("Description")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    splitter->addWidget(table_);

    details_ = new QPlainTextEdit(splitter);
    details_->setReadOnly(true);
    details_->setFont(monospaceFont(-1));
    details_->setPlaceholderText(QStringLiteral("Select a record to see its structured detail."));
    splitter->addWidget(details_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    summary_ = new QLabel(this);
    summary_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    layout->addWidget(summary_);

    connect(search_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(actionFilter_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(currentCaseOnly_, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(table_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { showDetails(row); });
    connect(context_, &ApplicationContext::auditChanged, this, &AuditPanel::refresh);
    connect(context_, &ApplicationContext::caseOpened, this, &AuditPanel::refresh);

    refresh();
}

void AuditPanel::refresh() {
    AuditQuery query;
    query.text = search_->text().trimmed().toStdString();
    if (const QString action = actionFilter_->currentData().toString(); !action.isEmpty()) {
        query.action = auditActionFromString(action.toStdString());
    }
    if (currentCaseOnly_->isChecked() && context_->currentCase()) {
        query.caseId = context_->currentCase()->id;
    }
    query.limit = 2000;

    auto listed = context_->audit().list(query);
    if (!listed) {
        summary_->setText(QStringLiteral("Unable to read the audit trail: %1")
                              .arg(QString::fromStdString(listed.error().toString())));
        return;
    }
    events_ = listed.take();

    table_->setRowCount(static_cast<int>(events_.size()));
    for (int row = 0; row < static_cast<int>(events_.size()); ++row) {
        const AuditEvent& event = events_[static_cast<std::size_t>(row)];
        auto* sequence = new QTableWidgetItem(QString::number(event.sequence));
        sequence->setFont(monospaceFont(-1));
        sequence->setForeground(colors::kTextDisabled);
        table_->setItem(row, 0, sequence);

        auto* time = new QTableWidgetItem(timestamp(event.occurredAt));
        time->setFont(monospaceFont(-1));
        table_->setItem(row, 1, time);

        auto* action = new QTableWidgetItem(QString::fromUtf8(toDisplayString(event.action)));
        action->setForeground(outcomeColour(event.outcome));
        table_->setItem(row, 2, action);

        table_->setItem(row, 3,
                        new QTableWidgetItem(QString::fromStdString(event.caseNumber.value_or(""))));
        table_->setItem(
            row, 4, new QTableWidgetItem(QString::fromStdString(event.evidenceNumber.value_or(""))));

        auto* description = new QTableWidgetItem(QString::fromStdString(event.description));
        description->setForeground(outcomeColour(event.outcome));
        table_->setItem(row, 5, description);
    }
    table_->resizeColumnsToContents();
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);

    auto total = context_->audit().count();
    summary_->setText(QStringLiteral("%1 record(s) shown · %2 in the database")
                          .arg(events_.size())
                          .arg(total ? total.value() : 0));

    if (!events_.empty()) {
        table_->selectRow(0);
        showDetails(0);
    } else {
        details_->clear();
    }
}

void AuditPanel::showDetails(int row) {
    if (row < 0 || row >= static_cast<int>(events_.size())) {
        details_->clear();
        return;
    }
    const AuditEvent& event = events_[static_cast<std::size_t>(row)];
    QString text;
    text += QStringLiteral("Record ....... %1\n").arg(QString::fromStdString(event.id));
    text += QStringLiteral("Sequence ..... %1\n").arg(event.sequence);
    text += QStringLiteral("Occurred ..... %1\n").arg(timestamp(event.occurredAt));
    text += QStringLiteral("Action ....... %1 (%2)\n")
                .arg(QString::fromUtf8(toDisplayString(event.action)),
                     QString::fromUtf8(toString(event.action)));
    text += QStringLiteral("Outcome ...... %1\n").arg(QString::fromStdString(event.outcome));
    text += QStringLiteral("Actor ........ %1\n").arg(QString::fromStdString(event.actor));
    text += QStringLiteral("Host ......... %1\n").arg(QString::fromStdString(event.host));
    text += QStringLiteral("Application .. TRACE %1\n").arg(QString::fromStdString(event.appVersion));
    if (event.caseNumber) {
        text += QStringLiteral("Case ......... %1\n").arg(QString::fromStdString(*event.caseNumber));
    }
    if (event.evidenceNumber) {
        text += QStringLiteral("Evidence ..... %1\n")
                    .arg(QString::fromStdString(*event.evidenceNumber));
    }
    text += QStringLiteral("\n%1\n\nDetails:\n%2")
                .arg(QString::fromStdString(event.description),
                     QString::fromStdString(event.detailsJson));
    details_->setPlainText(text);
}

}  // namespace trace::ui
