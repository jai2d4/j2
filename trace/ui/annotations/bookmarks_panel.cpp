#include "ui/annotations/bookmarks_panel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/annotations/bookmark_dialog.h"
#include "ui/app/application_context.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"

namespace trace::ui {

BookmarksPanel::BookmarksPanel(ApplicationContext* context, QWidget* parent)
    : QWidget(parent), context_(context) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("Timecode"), QStringLiteral("Title")});
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(tree_, 1);

    auto* buttons = new QHBoxLayout();
    addButton_ = new QPushButton(QStringLiteral("Add"), this);
    editButton_ = new QPushButton(QStringLiteral("Edit"), this);
    deleteButton_ = new QPushButton(QStringLiteral("Delete"), this);
    editButton_->setEnabled(false);
    deleteButton_->setEnabled(false);
    buttons->addWidget(addButton_);
    buttons->addWidget(editButton_);
    buttons->addWidget(deleteButton_);
    buttons->addStretch();
    layout->addLayout(buttons);

    summary_ = new QLabel(this);
    summary_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    layout->addWidget(summary_);

    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        const bool has = tree_->currentItem() != nullptr;
        editButton_->setEnabled(has);
        deleteButton_->setEnabled(has);
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr) return;
        emit jumpRequested(item->data(0, Qt::UserRole + 1).toLongLong());
    });
    connect(addButton_, &QPushButton::clicked, this, [this] { emit addRequested(); });
    connect(editButton_, &QPushButton::clicked, this, &BookmarksPanel::editSelected);
    connect(deleteButton_, &QPushButton::clicked, this, &BookmarksPanel::deleteSelected);
    connect(context_, &ApplicationContext::currentEvidenceChanged, this, &BookmarksPanel::refresh);
    connect(context_, &ApplicationContext::bookmarksChanged, this, &BookmarksPanel::refresh);

    refresh();
}

void BookmarksPanel::refresh() {
    tree_->clear();
    bookmarks_.clear();

    const auto& evidence = context_->currentEvidence();
    addButton_->setEnabled(evidence.has_value());
    if (!evidence) {
        summary_->setText(QStringLiteral("Open evidence in the viewer to add bookmarks."));
        return;
    }

    auto listed = context_->annotations().bookmarksForEvidence(evidence->id);
    if (!listed) {
        summary_->setText(QStringLiteral("Unable to read bookmarks: %1")
                              .arg(QString::fromStdString(listed.error().toString())));
        return;
    }
    bookmarks_ = listed.take();

    for (const auto& bookmark : bookmarks_) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, timecode(bookmark.timestampUs));
        item->setFont(0, monospaceFont(-1));
        item->setForeground(0, colors::kBookmark);
        item->setText(1, QString::fromStdString(bookmark.title));
        if (!bookmark.note.empty()) {
            item->setToolTip(1, QString::fromStdString(bookmark.note));
        }
        item->setData(0, Qt::UserRole, QString::fromStdString(bookmark.id));
        item->setData(0, Qt::UserRole + 1, static_cast<qlonglong>(bookmark.timestampUs));
    }
    tree_->resizeColumnToContents(0);

    summary_->setText(bookmarks_.empty()
                          ? QStringLiteral("No bookmarks yet — double-click one to jump to it.")
                          : QStringLiteral("%1 bookmark(s)").arg(bookmarks_.size()));
}

std::optional<Bookmark> BookmarksPanel::selected() const {
    auto* item = tree_->currentItem();
    if (item == nullptr) return std::nullopt;
    const QString id = item->data(0, Qt::UserRole).toString();
    for (const auto& bookmark : bookmarks_) {
        if (QString::fromStdString(bookmark.id) == id) return bookmark;
    }
    return std::nullopt;
}

void BookmarksPanel::selectBookmark(const QString& bookmarkId) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* item = tree_->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == bookmarkId) {
            tree_->setCurrentItem(item);
            return;
        }
    }
}

void BookmarksPanel::addBookmarkAt(qint64 positionUs, qint64 frameNumber) {
    const auto& evidence = context_->currentEvidence();
    if (!evidence) return;

    BookmarkDialog dialog(positionUs, std::nullopt, this);
    if (dialog.exec() != QDialog::Accepted) return;

    BookmarkDraft draft;
    draft.caseId = evidence->caseId;
    draft.evidenceId = evidence->id;
    draft.timestampUs = dialog.timestampUs();
    if (frameNumber >= 0 && dialog.timestampUs() == positionUs) draft.frameNumber = frameNumber;
    draft.title = dialog.title().toStdString();
    draft.note = dialog.note().toStdString();

    auto created = context_->annotations().createBookmark(
        draft, context_->currentCaseNumber().toStdString(), evidence->evidenceNumber);
    if (!created) {
        QMessageBox::critical(this, QStringLiteral("Bookmark not saved"),
                              QString::fromStdString(created.error().toString()));
        return;
    }
    refresh();
    context_->notifyAuditChanged();
    emit bookmarksChanged();
}

void BookmarksPanel::editSelected() {
    const auto bookmark = selected();
    if (!bookmark || !context_->currentEvidence()) return;

    BookmarkDialog dialog(bookmark->timestampUs, bookmark, this);
    if (dialog.exec() != QDialog::Accepted) return;

    Bookmark updated = *bookmark;
    updated.timestampUs = dialog.timestampUs();
    updated.title = dialog.title().toStdString();
    updated.note = dialog.note().toStdString();

    if (auto status = context_->annotations().updateBookmark(
            updated, context_->currentCaseNumber().toStdString(),
            context_->currentEvidence()->evidenceNumber);
        !status) {
        QMessageBox::critical(this, QStringLiteral("Bookmark not saved"),
                              QString::fromStdString(status.error().toString()));
        return;
    }
    refresh();
    context_->notifyAuditChanged();
    emit bookmarksChanged();
}

void BookmarksPanel::deleteSelected() {
    const auto bookmark = selected();
    if (!bookmark || !context_->currentEvidence()) return;

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Delete bookmark"),
        QStringLiteral("Delete the bookmark \"%1\" at %2?\n\nThe deletion is recorded in the audit "
                       "trail. Evidence is not affected.")
            .arg(QString::fromStdString(bookmark->title), timecode(bookmark->timestampUs)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    if (auto status = context_->annotations().deleteBookmark(
            *bookmark, context_->currentCaseNumber().toStdString(),
            context_->currentEvidence()->evidenceNumber);
        !status) {
        QMessageBox::critical(this, QStringLiteral("Bookmark not deleted"),
                              QString::fromStdString(status.error().toString()));
        return;
    }
    refresh();
    context_->notifyAuditChanged();
    emit bookmarksChanged();
}

}  // namespace trace::ui
