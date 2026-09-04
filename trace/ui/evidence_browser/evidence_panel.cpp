#include "ui/evidence_browser/evidence_panel.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "core/common/string_utils.h"
#include "ui/app/application_context.h"
#include "ui/common/ingest_dialog.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"

namespace trace::ui {

EvidencePanel::EvidencePanel(ApplicationContext* context, QWidget* parent)
    : QWidget(parent), context_(context) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout();
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Filter evidence"));
    search_->setClearButtonEnabled(true);
    header->addWidget(search_, 1);
    importButton_ = new QPushButton(QStringLiteral("Import"), this);
    styleAsPrimaryAction(importButton_);
    importButton_->setToolTip(
        QStringLiteral("Copy files into managed case storage, hash them and extract metadata"));
    header->addWidget(importButton_);
    layout->addLayout(header);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels(
        {QStringLiteral("Evidence"), QStringLiteral("Integrity"), QStringLiteral("Media")});
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(tree_, 1);

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    summary_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    layout->addWidget(summary_);

    connect(importButton_, &QPushButton::clicked, this, &EvidencePanel::importEvidence);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &EvidencePanel::onSelectionChanged);
    connect(search_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(context_, &ApplicationContext::evidenceChanged, this, &EvidencePanel::refresh);
    connect(context_, &ApplicationContext::caseOpened, this, &EvidencePanel::refresh);
    connect(context_, &ApplicationContext::caseClosed, this, &EvidencePanel::refresh);

    refresh();
}

void EvidencePanel::refresh() {
    if (!context_->isInitialised()) return;
    const auto& openCase = context_->currentCase();
    importButton_->setEnabled(openCase.has_value());

    const QString previousId =
        context_->currentEvidence() ? QString::fromStdString(context_->currentEvidence()->id)
                                    : QString();

    updating_ = true;
    tree_->clear();
    evidence_.clear();

    if (!openCase) {
        summary_->setText(QStringLiteral("Open a case to see its evidence."));
        updating_ = false;
        return;
    }

    auto listed = context_->evidence().listForCase(openCase->id);
    if (!listed) {
        summary_->setText(QStringLiteral("Unable to read evidence: %1")
                              .arg(QString::fromStdString(listed.error().toString())));
        updating_ = false;
        return;
    }

    const std::string filter = search_->text().trimmed().toStdString();
    for (auto& item : listed.value()) {
        if (!filter.empty() && !containsCaseInsensitive(item.originalFilename, filter) &&
            !containsCaseInsensitive(item.evidenceNumber, filter) &&
            !containsCaseInsensitive(item.description, filter)) {
            continue;
        }
        evidence_.push_back(item);
    }

    QTreeWidgetItem* toSelect = nullptr;
    for (const auto& item : evidence_) {
        auto* row = new QTreeWidgetItem(tree_);
        row->setText(0, QStringLiteral("%1  %2")
                            .arg(QString::fromStdString(item.evidenceNumber),
                                 QString::fromStdString(item.originalFilename)));
        row->setToolTip(0, QStringLiteral("%1\n%2\n%3")
                               .arg(QString::fromStdString(item.originalFilename),
                                    fileSize(item.fileSize),
                                    QString::fromStdString(item.sha256)));
        row->setText(1, QString::fromUtf8(toDisplayString(item.integrityStatus)));
        row->setForeground(1, integrityColour(item.integrityStatus));
        row->setText(2, QString::fromUtf8(toDisplayString(item.mediaStatus)));
        row->setForeground(2, mediaStatusColour(item.mediaStatus));
        row->setData(0, Qt::UserRole, QString::fromStdString(item.id));
        if (!previousId.isEmpty() && previousId == QString::fromStdString(item.id)) toSelect = row;
    }

    tree_->resizeColumnToContents(1);
    tree_->resizeColumnToContents(2);
    summary_->setText(evidence_.empty()
                          ? QStringLiteral("No evidence in this case yet.")
                          : QStringLiteral("%1 item(s) in managed storage").arg(evidence_.size()));

    updating_ = false;
    if (toSelect != nullptr) {
        tree_->setCurrentItem(toSelect);
    }
}

void EvidencePanel::onSelectionChanged() {
    if (updating_) return;
    auto* item = tree_->currentItem();
    if (item == nullptr) return;
    const QString id = item->data(0, Qt::UserRole).toString();

    // Refreshing the list restores the current selection; that is not the
    // operator opening something, so nothing should be re-opened.
    if (context_->currentEvidence() &&
        QString::fromStdString(context_->currentEvidence()->id) == id) {
        return;
    }

    for (const auto& candidate : evidence_) {
        if (QString::fromStdString(candidate.id) != id) continue;
        context_->setCurrentEvidence(candidate);
        emit evidenceActivated(candidate);
        return;
    }
}

void EvidencePanel::selectEvidence(const QString& evidenceId) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* item = tree_->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == evidenceId) {
            tree_->setCurrentItem(item);
            return;
        }
    }
}

void EvidencePanel::importEvidence() {
    if (!context_->currentCase()) {
        QMessageBox::information(this, QStringLiteral("No case open"),
                                 QStringLiteral("Open or create a case before importing evidence."));
        return;
    }

    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Select evidence to import"), QString(),
        QStringLiteral("Media files (*.mp4 *.mov *.avi *.mkv *.webm *.m4v *.mpg *.mpeg *.ts "
                       "*.m2ts *.wmv *.flv *.3gp *.mxf *.wav *.mp3 *.m4a *.aac *.flac *.ogg "
                       "*.jpg *.jpeg *.png *.bmp *.tif *.tiff);;All files (*)"));
    if (files.isEmpty()) return;

    IngestDialog dialog(context_, files, this);
    dialog.exec();

    refresh();
    context_->notifyAuditChanged();
    if (dialog.lastImported()) {
        selectEvidence(QString::fromStdString(dialog.lastImported()->id));
    }
}

}  // namespace trace::ui
