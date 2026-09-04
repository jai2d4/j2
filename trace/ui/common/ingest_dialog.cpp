#include "ui/common/ingest_dialog.h"

#include <QCloseEvent>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "core/common/logging.h"
#include "ui/app/application_context.h"
#include "ui/common/background_task.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"

namespace trace::ui {
namespace {

const std::vector<IngestStage> kDisplayedStages = {
    IngestStage::Validating,        IngestStage::PreparingStorage, IngestStage::CopyingOriginal,
    IngestStage::VerifyingCopy,     IngestStage::ExtractingMetadata,
    IngestStage::CreatingRecord,    IngestStage::Complete};

int stageRow(IngestStage stage) {
    for (std::size_t i = 0; i < kDisplayedStages.size(); ++i) {
        if (kDisplayedStages[i] == stage) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

IngestDialog::IngestDialog(ApplicationContext* context, QStringList sourcePaths, QWidget* parent)
    : QDialog(parent), context_(context), sources_(std::move(sourcePaths)) {
    setWindowTitle(QStringLiteral("Importing evidence"));
    setModal(true);
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("IMPORTING EVIDENCE"), this);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                        "letter-spacing: 1.5px;")
                             .arg(colors::kTextSecondary.name()));
    layout->addWidget(title);

    fileLabel_ = new QLabel(this);
    fileLabel_->setWordWrap(true);
    fileLabel_->setStyleSheet(
        QStringLiteral("font-size: 14px; color: %1;").arg(colors::kTextPrimary.name()));
    layout->addWidget(fileLabel_);

    stageList_ = new QListWidget(this);
    stageList_->setSelectionMode(QAbstractItemView::NoSelection);
    stageList_->setFocusPolicy(Qt::NoFocus);
    stageList_->setFixedHeight(190);
    for (const auto stage : kDisplayedStages) {
        stageList_->addItem(QStringLiteral("   %1").arg(QString::fromUtf8(toDisplayString(stage))));
    }
    layout->addWidget(stageList_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    layout->addWidget(progressBar_);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setWordWrap(true);
    summaryLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;").arg(colors::kTextSecondary.name()));
    layout->addWidget(summaryLabel_);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
    closeButton_ = new QPushButton(QStringLiteral("Close"), this);
    styleAsPrimaryAction(closeButton_);
    closeButton_->setEnabled(false);
    buttons->addWidget(cancelButton_);
    buttons->addWidget(closeButton_);
    layout->addLayout(buttons);

    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        if (task_ != nullptr) task_->requestCancellation();
        cancelButton_->setEnabled(false);
        summaryLabel_->setText(QStringLiteral("Cancelling — the original source file is untouched."));
    });
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::accept);

    startNext();
}

IngestDialog::~IngestDialog() {
    if (task_ != nullptr) {
        task_->requestCancellation();
        task_->wait();
    }
}

void IngestDialog::closeEvent(QCloseEvent* event) {
    if (!finished_ && task_ != nullptr && task_->running()) {
        task_->requestCancellation();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void IngestDialog::markStage(IngestStage stage) {
    const int row = stageRow(stage);
    for (int i = 0; i < stageList_->count(); ++i) {
        auto* item = stageList_->item(i);
        const QString label = QString::fromUtf8(toDisplayString(kDisplayedStages[static_cast<std::size_t>(i)]));
        if (row < 0) continue;
        if (i < row) {
            item->setText(QStringLiteral(" ✓ %1").arg(label));
            item->setForeground(colors::kVerified);
        } else if (i == row) {
            item->setText(QStringLiteral(" ▸ %1").arg(label));
            item->setForeground(colors::kAccent);
        } else {
            item->setText(QStringLiteral("   %1").arg(label));
            item->setForeground(colors::kTextDisabled);
        }
    }
}

void IngestDialog::appendFailure(const QString& file, const QString& message) {
    failures_ << QStringLiteral("%1: %2").arg(file, message);
}

void IngestDialog::startNext() {
    if (index_ >= sources_.size()) {
        finished_ = true;
        cancelButton_->setEnabled(false);
        closeButton_->setEnabled(true);
        closeButton_->setFocus();
        progressBar_->setValue(100);

        QString summary =
            QStringLiteral("%1 of %2 file(s) imported.").arg(imported_).arg(sources_.size());
        if (!failures_.isEmpty()) {
            summary += QStringLiteral("\n\nNot imported (original sources were not modified):\n• ") +
                       failures_.join(QStringLiteral("\n• "));
            summaryLabel_->setStyleSheet(
                QStringLiteral("color: %1; font-size: 12px;").arg(colors::kWarning.name()));
        }
        summaryLabel_->setText(summary);
        return;
    }

    const QString source = sources_.at(index_);
    fileLabel_->setText(source);
    markStage(IngestStage::Validating);
    progressBar_->setValue(0);

    IngestRequest request;
    request.caseId = context_->currentCase()->id;
    request.sourcePath = std::filesystem::path(source.toStdString());

    task_ = std::make_unique<BackgroundTask>();

    connect(task_.get(), &BackgroundTask::progressChanged, this,
            [this](int percent, const QString& stage, const QString& message) {
                progressBar_->setValue(percent);
                const int row = stage.toInt();
                if (row >= 0 && row < static_cast<int>(kDisplayedStages.size())) {
                    markStage(kDisplayedStages[static_cast<std::size_t>(row)]);
                }
                summaryLabel_->setText(message);
            });

    connect(task_.get(), &BackgroundTask::finished, this,
            [this, source](bool succeeded, const QString& message) {
                if (succeeded) {
                    ++imported_;
                    markStage(IngestStage::Complete);
                    for (int i = 0; i < stageList_->count(); ++i) {
                        stageList_->item(i)->setForeground(colors::kVerified);
                        stageList_->item(i)->setText(QStringLiteral(" ✓ %1").arg(
                            QString::fromUtf8(toDisplayString(
                                kDisplayedStages[static_cast<std::size_t>(i)]))));
                    }
                    context_->notifyEvidenceChanged();
                    context_->notifyAuditChanged();
                } else {
                    appendFailure(QFileInfo(source).fileName(), message);
                }
                ++index_;
                // The finished signal is delivered on the GUI thread while the
                // worker is winding down; join before starting the next file.
                if (task_ != nullptr) task_->wait();
                startNext();
            });

    auto* context = context_;
    task_->start([this, request, context](BackgroundTask& task) {
        auto outcome = context->evidence().ingest(request, [&task](const IngestProgress& progress) {
            const int row = stageRow(progress.stage);
            QString message = QString::fromStdString(progress.message);
            if (progress.bytesTotal > 0 && progress.stage == IngestStage::CopyingOriginal) {
                message += QStringLiteral(" — %1 of %2")
                               .arg(fileSize(progress.bytesProcessed), fileSize(progress.bytesTotal));
            }
            task.reportProgress(static_cast<int>(progress.overallFraction * 100.0),
                                QString::number(row), message);
            return !task.cancellationRequested();
        });

        if (!outcome) {
            task.reportFinished(false, QString::fromStdString(outcome.error().message()));
            return;
        }
        IngestOutcome result = outcome.take();
        lastImported_ = result.evidence;
        QString message = QStringLiteral("%1 imported. SHA-256 %2")
                              .arg(QString::fromStdString(result.evidence.evidenceNumber),
                                   QString::fromStdString(result.evidence.sha256.substr(0, 16)) +
                                       QStringLiteral("…"));
        if (!result.metadataExtracted) {
            message += QStringLiteral("\nMetadata could not be read: ") +
                       QString::fromStdString(result.metadataError.empty()
                                                  ? "the container could not be parsed"
                                                  : result.metadataError) +
                       QStringLiteral("\nThe stored original is intact and hashed.");
        }
        if (result.duplicateOfExisting) {
            message += QStringLiteral("\nNote: identical content is already in this case as ") +
                       QString::fromStdString(result.duplicateEvidenceNumber) + QStringLiteral(".");
        }
        task.reportFinished(true, message);
    });
}

}  // namespace trace::ui
