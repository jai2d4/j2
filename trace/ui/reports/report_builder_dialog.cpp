#include "ui/reports/report_builder_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/app/application_context.h"
#include "ui/common/background_task.h"
#include "ui/common/display_utils.h"
#include "ui/common/theme.h"

namespace trace::ui {
namespace {

constexpr int kProgressPollMs = 120;
/// Beyond this the list stops being something an operator can meaningfully review.
constexpr int kMaxListedDetections = 2000;

}  // namespace

ReportBuilderDialog::ReportBuilderDialog(ApplicationContext* context, QWidget* parent)
    : QDialog(parent), context_(context) {
    setWindowTitle(QStringLiteral("Export exhibit bundle"));
    resize(860, 720);
    buildUi();

    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(kProgressPollMs);
    connect(progressTimer_, &QTimer::timeout, this, &ReportBuilderDialog::applyProgressToUi);

    reloadEvidence();
    reloadDetections();
    updateSummary();
    updateControlState();
}

ReportBuilderDialog::~ReportBuilderDialog() {
    if (cancellation_ != nullptr) cancellation_->store(true);
    if (task_ != nullptr) {
        task_->requestCancellation();
        task_->wait();
    }
}

void ReportBuilderDialog::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    titleField_ = new QLineEdit(this);
    titleField_->setPlaceholderText(QStringLiteral("Report title, e.g. Findings for review"));
    titleField_->setText(QStringLiteral("Findings"));
    layout->addWidget(new QLabel(QStringLiteral("Title"), this));
    layout->addWidget(titleField_);

    // ----------------------------------------------------------- evidence
    auto* evidenceBox = new QGroupBox(QStringLiteral("Evidence to include"), this);
    auto* evidenceLayout = new QVBoxLayout(evidenceBox);
    evidenceTree_ = new QTreeWidget(evidenceBox);
    evidenceTree_->setColumnCount(3);
    evidenceTree_->setHeaderLabels(
        {QStringLiteral("Item"), QStringLiteral("Filename"), QStringLiteral("Integrity")});
    evidenceTree_->setRootIsDecorated(false);
    evidenceTree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    evidenceLayout->addWidget(evidenceTree_);
    layout->addWidget(evidenceBox, 1);

    // --------------------------------------------------------- detections
    auto* detectionBox = new QGroupBox(QStringLiteral("Findings to cite"), this);
    auto* detectionLayout = new QVBoxLayout(detectionBox);

    unconfirmedBox_ = new QCheckBox(
        QStringLiteral("Include detections no analyst has confirmed"), detectionBox);
    unconfirmedBox_->setChecked(false);
    unconfirmedBox_->setToolTip(
        QStringLiteral("Off by default. A confirmed detection records that a named analyst "
                       "agreed with it. Including unconfirmed ones is allowed, but the report "
                       "will say so where a reader cannot miss it."));
    detectionLayout->addWidget(unconfirmedBox_);

    detectionTree_ = new QTreeWidget(detectionBox);
    detectionTree_->setColumnCount(5);
    detectionTree_->setHeaderLabels({QStringLiteral("Item"), QStringLiteral("Time"),
                                     QStringLiteral("Class"), QStringLiteral("Confidence"),
                                     QStringLiteral("Review")});
    detectionTree_->setRootIsDecorated(false);
    detectionTree_->setAlternatingRowColors(true);
    detectionLayout->addWidget(detectionTree_);
    layout->addWidget(detectionBox, 2);

    noticeLabel_ = new QLabel(this);
    noticeLabel_->setWordWrap(true);
    noticeLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    noticeLabel_->setText(QStringLiteral(
        "The bundle records what TRACE observed and what was done to this material. It states "
        "no conclusions, and nothing in it is digitally signed — the manifests are integrity "
        "digests."));
    layout->addWidget(noticeLabel_);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setStyleSheet(
        QStringLiteral("color: %1;").arg(colors::kTextSecondary.name()));
    layout->addWidget(summaryLabel_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->setVisible(false);
    layout->addWidget(progressBar_);

    progressLabel_ = new QLabel(this);
    progressLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    layout->addWidget(progressLabel_);

    auto* buttons = new QHBoxLayout();
    exportButton_ = new QPushButton(QStringLiteral("Export bundle…"), this);
    styleAsPrimaryAction(exportButton_);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel export"), this);
    cancelButton_->setEnabled(false);
    closeButton_ = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(exportButton_);
    buttons->addWidget(cancelButton_);
    buttons->addStretch();
    buttons->addWidget(closeButton_);
    layout->addLayout(buttons);

    connect(exportButton_, &QPushButton::clicked, this, &ReportBuilderDialog::startExport);
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        if (!exportInFlight_) return;
        if (cancellation_ != nullptr) cancellation_->store(true);
        if (task_ != nullptr) task_->requestCancellation();
        cancelButton_->setEnabled(false);
        progressLabel_->setText(QStringLiteral("Cancelling…"));
    });
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::reject);

    const auto selectionChanged = [this] {
        updateSummary();
        updateControlState();
    };
    connect(evidenceTree_, &QTreeWidget::itemChanged, this, selectionChanged);
    connect(detectionTree_, &QTreeWidget::itemChanged, this, selectionChanged);
    connect(unconfirmedBox_, &QCheckBox::toggled, this, [this] {
        reloadDetections();
        updateSummary();
        updateControlState();
    });
}

void ReportBuilderDialog::reloadEvidence() {
    evidenceTree_->clear();
    evidence_.clear();
    if (!context_->isInitialised() || !context_->currentCase()) return;

    auto listed = context_->evidence().listForCase(context_->currentCase()->id);
    if (!listed) return;
    evidence_ = listed.take();

    const auto* current = context_->currentEvidence() ? &*context_->currentEvidence() : nullptr;
    for (const auto& item : evidence_) {
        auto* row = new QTreeWidgetItem(evidenceTree_);
        row->setText(0, QString::fromStdString(item.evidenceNumber));
        row->setText(1, QString::fromStdString(item.originalFilename));
        row->setText(2, QString::fromUtf8(toDisplayString(item.integrityStatus)));
        row->setForeground(2, QBrush(integrityColour(item.integrityStatus)));
        row->setData(0, Qt::UserRole, QString::fromStdString(item.id));
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        // The item in the viewer is the one the operator is working on.
        row->setCheckState(0, current != nullptr && current->id == item.id ? Qt::Checked
                                                                          : Qt::Unchecked);
    }
}

void ReportBuilderDialog::reloadDetections() {
    detectionTree_->clear();
    detections_.clear();
    if (!context_->isInitialised() || !context_->currentCase()) return;

    DetectionQuery query;
    query.caseId = context_->currentCase()->id;
    query.includeRejected = false;
    if (!unconfirmedBox_->isChecked()) query.verification = DetectionVerification::Confirmed;
    query.limit = kMaxListedDetections;

    auto listed = context_->analysis().detections(query);
    if (!listed) return;
    detections_ = listed.take();

    const auto numberFor = [this](const std::string& evidenceId) {
        for (const auto& item : evidence_) {
            if (item.id == evidenceId) return item.evidenceNumber;
        }
        return std::string("—");
    };

    for (const auto& detection : detections_) {
        auto* row = new QTreeWidgetItem(detectionTree_);
        row->setText(0, QString::fromStdString(numberFor(detection.evidenceId)));
        row->setText(1, timecode(detection.timestampUs));
        row->setFont(1, monospaceFont(-1));
        row->setText(2, QString::fromStdString(detection.classLabel));
        row->setText(3, QStringLiteral("%1%").arg(detection.confidence * 100.0, 0, 'f', 1));
        row->setText(4, QString::fromUtf8(toDisplayString(detection.verification)));
        row->setData(0, Qt::UserRole, QString::fromStdString(detection.id));
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        row->setCheckState(0, Qt::Unchecked);
    }
}

ReportDraft ReportBuilderDialog::buildDraft() const {
    ReportDraft draft;
    if (context_->currentCase()) draft.caseId = context_->currentCase()->id;
    draft.title = titleField_->text().trimmed().toStdString();
    draft.includeUnconfirmedDetections = unconfirmedBox_->isChecked();

    for (int i = 0; i < evidenceTree_->topLevelItemCount(); ++i) {
        auto* row = evidenceTree_->topLevelItem(i);
        if (row->checkState(0) != Qt::Checked) continue;
        draft.evidenceIds.push_back(row->data(0, Qt::UserRole).toString().toStdString());
    }
    for (int i = 0; i < detectionTree_->topLevelItemCount(); ++i) {
        auto* row = detectionTree_->topLevelItem(i);
        if (row->checkState(0) != Qt::Checked) continue;
        draft.detectionIds.push_back(row->data(0, Qt::UserRole).toString().toStdString());
    }
    return draft;
}

void ReportBuilderDialog::updateSummary() {
    const ReportDraft draft = buildDraft();
    QString text = QStringLiteral("%1 evidence item%2, %3 detection%4 selected")
                       .arg(draft.evidenceIds.size())
                       .arg(draft.evidenceIds.size() == 1 ? QString() : QStringLiteral("s"))
                       .arg(draft.detectionIds.size())
                       .arg(draft.detectionIds.size() == 1 ? QString() : QStringLiteral("s"));
    if (draft.includeUnconfirmedDetections) {
        text += QStringLiteral("  ·  unconfirmed detections are being offered");
    }
    summaryLabel_->setText(text);
}

void ReportBuilderDialog::updateControlState() {
    const ReportDraft draft = buildDraft();
    const bool ready = !exportInFlight_ && !draft.caseId.empty() && !draft.title.empty() &&
                       !draft.evidenceIds.empty();
    exportButton_->setEnabled(ready);
    cancelButton_->setEnabled(exportInFlight_);
    titleField_->setEnabled(!exportInFlight_);
    unconfirmedBox_->setEnabled(!exportInFlight_);
    evidenceTree_->setEnabled(!exportInFlight_);
    detectionTree_->setEnabled(!exportInFlight_);
    closeButton_->setEnabled(!exportInFlight_);

    exportButton_->setToolTip(draft.evidenceIds.empty()
                                  ? QStringLiteral("Select at least one evidence item.")
                                  : QStringLiteral("Write the bundle and verify it."));
}

void ReportBuilderDialog::startExport() {
    if (exportInFlight_ || !context_->isInitialised()) return;

    const ReportDraft draft = buildDraft();
    if (draft.evidenceIds.empty()) {
        emit statusMessage(QStringLiteral("Select at least one evidence item first."), 5000);
        return;
    }

    QString destination = destinationOverride_;
    if (destination.isEmpty()) {
        destination = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Where should the exhibit bundle be written?"));
        if (destination.isEmpty()) return;
    }

    auto created = context_->reports().createReport(draft);
    if (!created) {
        QMessageBox::warning(this, QStringLiteral("Report not created"),
                             QString::fromStdString(created.error().message()));
        return;
    }
    const std::string reportId = created.value().id;

    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        latestProgress_ = ExportProgress{};
        outcome_.reset();
    }

    cancellation_ = std::make_shared<std::atomic<bool>>(false);
    exportInFlight_ = true;
    progressBar_->setVisible(true);
    progressBar_->setValue(0);
    progressLabel_->setText(QStringLiteral("Starting…"));
    updateControlState();

    task_ = std::make_unique<BackgroundTask>();
    connect(task_.get(), &BackgroundTask::finished, this,
            [this](bool succeeded, const QString& message) {
                exportInFlight_ = false;
                if (task_ != nullptr) {
                    task_->wait();
                    task_.reset();
                }
                progressTimer_->stop();
                cancellation_.reset();

                std::optional<ExportOutcome> result;
                {
                    std::lock_guard<std::mutex> lock(progressMutex_);
                    result = outcome_;
                }

                progressBar_->setValue(succeeded ? 1000 : progressBar_->value());
                progressLabel_->setText(message);
                progressLabel_->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                        .arg((succeeded ? colors::kVerified : colors::kFailure).name()));

                context_->notifyReportsChanged();
                context_->notifyDerivedAssetsChanged();
                context_->notifyAuditChanged();
                updateControlState();

                emit statusMessage(message, 12000);
                emit exportFinished(succeeded, message);

                if (!succeeded && result && result->status == ReportStatus::Failed) {
                    QMessageBox::warning(this, QStringLiteral("Bundle not exported"), message);
                }
            });

    auto* service = &context_->reports();
    auto token = cancellation_;
    const std::filesystem::path root = destination.toStdString();

    progressTimer_->start();
    emit statusMessage(QStringLiteral("Writing the exhibit bundle…"), 0);

    task_->start([this, service, reportId, root, token](BackgroundTask& task) {
        const auto callback = [this, &task](const ExportProgress& progress) {
            {
                std::lock_guard<std::mutex> lock(progressMutex_);
                latestProgress_ = progress;
            }
            return !task.cancellationRequested();
        };

        auto exported = service->exportReport(reportId, root, callback, token);
        if (!exported) {
            task.reportFinished(false, QStringLiteral("Export failed: %1")
                                           .arg(QString::fromStdString(exported.error().message())));
            return;
        }

        const ExportOutcome outcome = exported.take();
        {
            std::lock_guard<std::mutex> lock(progressMutex_);
            outcome_ = outcome;
        }

        // Only the service's own verified result decides what is reported here.
        switch (outcome.status) {
            case ReportStatus::Exported:
                task.reportFinished(
                    true, QStringLiteral("Bundle exported and verified — %1 files, every digest "
                                         "matched.\n%2")
                              .arg(outcome.fileCount)
                              .arg(QString::fromStdString(outcome.bundlePath.string())));
                break;
            case ReportStatus::Cancelled:
                task.reportFinished(false,
                                    QStringLiteral("Export cancelled. No bundle was written."));
                break;
            default:
                task.reportFinished(
                    false, QStringLiteral("Export failed: %1")
                               .arg(QString::fromStdString(
                                   outcome.error.value_or("no reason recorded"))));
                break;
        }
    });
}

void ReportBuilderDialog::applyProgressToUi() {
    ExportProgress progress;
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        progress = latestProgress_;
    }
    if (progress.totalSteps == 0) return;

    progressBar_->setValue(static_cast<int>(progress.fraction() * 1000.0));
    progressLabel_->setText(QStringLiteral("%1 — step %2 of %3")
                                .arg(QString::fromStdString(progress.stage))
                                .arg(progress.completedSteps)
                                .arg(progress.totalSteps));
}

}  // namespace trace::ui
