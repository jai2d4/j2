#include "ui/annotations/annotations_panel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/annotations/annotation_dialog.h"
#include "ui/app/application_context.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"

namespace trace::ui {

AnnotationsPanel::AnnotationsPanel(ApplicationContext* context, QWidget* parent)
    : QWidget(parent), context_(context) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels(
        {QStringLiteral("Start"), QStringLiteral("End"), QStringLiteral("Note")});
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
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
    connect(editButton_, &QPushButton::clicked, this, &AnnotationsPanel::editSelected);
    connect(deleteButton_, &QPushButton::clicked, this, &AnnotationsPanel::deleteSelected);
    connect(context_, &ApplicationContext::currentEvidenceChanged, this, &AnnotationsPanel::refresh);
    connect(context_, &ApplicationContext::annotationsChanged, this, &AnnotationsPanel::refresh);

    refresh();
}

void AnnotationsPanel::refresh() {
    tree_->clear();
    annotations_.clear();

    const auto& evidence = context_->currentEvidence();
    addButton_->setEnabled(evidence.has_value());
    if (!evidence) {
        summary_->setText(QStringLiteral("Open evidence in the viewer to add notes."));
        return;
    }

    auto listed = context_->annotations().annotationsForEvidence(evidence->id);
    if (!listed) {
        summary_->setText(QStringLiteral("Unable to read notes: %1")
                              .arg(QString::fromStdString(listed.error().toString())));
        return;
    }
    annotations_ = listed.take();

    for (const auto& annotation : annotations_) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, timecode(annotation.startUs));
        item->setFont(0, monospaceFont(-1));
        item->setForeground(0, colors::kAnnotation);
        item->setText(1, annotation.endUs ? timecode(*annotation.endUs) : QStringLiteral("—"));
        item->setFont(1, monospaceFont(-1));
        QString text = QString::fromStdString(annotation.text);
        item->setToolTip(2, text);
        text.replace(QLatin1Char('\n'), QLatin1Char(' '));
        item->setText(2, text);
        item->setData(0, Qt::UserRole, QString::fromStdString(annotation.id));
        item->setData(0, Qt::UserRole + 1, static_cast<qlonglong>(annotation.startUs));
    }
    tree_->resizeColumnToContents(0);
    tree_->resizeColumnToContents(1);

    summary_->setText(annotations_.empty()
                          ? QStringLiteral("No notes yet — double-click one to jump to it.")
                          : QStringLiteral("%1 note(s)").arg(annotations_.size()));
}

std::optional<Annotation> AnnotationsPanel::selected() const {
    auto* item = tree_->currentItem();
    if (item == nullptr) return std::nullopt;
    const QString id = item->data(0, Qt::UserRole).toString();
    for (const auto& annotation : annotations_) {
        if (QString::fromStdString(annotation.id) == id) return annotation;
    }
    return std::nullopt;
}

void AnnotationsPanel::addAnnotationAt(qint64 positionUs, qint64 frameNumber) {
    const auto& evidence = context_->currentEvidence();
    if (!evidence) return;

    AnnotationDialog dialog(positionUs, std::nullopt, this);
    if (dialog.exec() != QDialog::Accepted) return;

    AnnotationDraft draft;
    draft.caseId = evidence->caseId;
    draft.evidenceId = evidence->id;
    draft.startUs = dialog.startUs();
    draft.endUs = dialog.endUs();
    draft.type = dialog.endUs() ? AnnotationType::RangeNote : AnnotationType::TimestampNote;
    if (frameNumber >= 0 && dialog.startUs() == positionUs) draft.startFrame = frameNumber;
    draft.text = dialog.text().toStdString();

    auto created = context_->annotations().createAnnotation(
        draft, context_->currentCaseNumber().toStdString(), evidence->evidenceNumber);
    if (!created) {
        QMessageBox::critical(this, QStringLiteral("Note not saved"),
                              QString::fromStdString(created.error().toString()));
        return;
    }
    refresh();
    context_->notifyAuditChanged();
    emit annotationsChanged();
}

void AnnotationsPanel::editSelected() {
    const auto annotation = selected();
    if (!annotation || !context_->currentEvidence()) return;

    AnnotationDialog dialog(annotation->startUs, annotation, this);
    if (dialog.exec() != QDialog::Accepted) return;

    Annotation updated = *annotation;
    updated.startUs = dialog.startUs();
    updated.endUs = dialog.endUs();
    updated.type = dialog.endUs() ? AnnotationType::RangeNote : AnnotationType::TimestampNote;
    updated.text = dialog.text().toStdString();

    if (auto status = context_->annotations().updateAnnotation(
            updated, context_->currentCaseNumber().toStdString(),
            context_->currentEvidence()->evidenceNumber);
        !status) {
        QMessageBox::critical(this, QStringLiteral("Note not saved"),
                              QString::fromStdString(status.error().toString()));
        return;
    }
    refresh();
    context_->notifyAuditChanged();
    emit annotationsChanged();
}

void AnnotationsPanel::deleteSelected() {
    const auto annotation = selected();
    if (!annotation || !context_->currentEvidence()) return;

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Delete note"),
        QStringLiteral("Delete the note at %1?\n\nThe deletion is recorded in the audit trail. "
                       "Evidence is not affected.")
            .arg(timecode(annotation->startUs)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    if (auto status = context_->annotations().deleteAnnotation(
            *annotation, context_->currentCaseNumber().toStdString(),
            context_->currentEvidence()->evidenceNumber);
        !status) {
        QMessageBox::critical(this, QStringLiteral("Note not deleted"),
                              QString::fromStdString(status.error().toString()));
        return;
    }
    refresh();
    context_->notifyAuditChanged();
    emit annotationsChanged();
}

}  // namespace trace::ui
