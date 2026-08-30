#include "ui/analysis/analysis_panel.h"

#include <QBrush>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <utility>

#include "ai/detection/detection_provider_registry.h"
#include "core/common/time_utils.h"
#include "ui/app/application_context.h"
#include "ui/common/background_task.h"
#include "ui/common/display_utils.h"
#include "ui/common/theme.h"

namespace trace::ui {
namespace {

constexpr int kProgressPollMs = 150;

const std::vector<AnalysisQuality> kQualities = {AnalysisQuality::Fast, AnalysisQuality::Standard,
                                                 AnalysisQuality::Detailed};
const std::vector<DevicePreference> kDevices = {DevicePreference::Auto, DevicePreference::Cpu,
                                               DevicePreference::Gpu};

QLabel* fieldLabel(QWidget* parent, const QString& text, const QColor& colour) {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(colour.name()));
    label->setWordWrap(true);
    return label;
}

QString statusColourName(AnalysisRunStatus status) {
    switch (status) {
        case AnalysisRunStatus::Completed:
            return colors::kVerified.name();
        case AnalysisRunStatus::Failed:
            return colors::kFailure.name();
        case AnalysisRunStatus::Cancelled:
        case AnalysisRunStatus::Partial:
            return colors::kWarning.name();
        case AnalysisRunStatus::Running:
        case AnalysisRunStatus::Queued:
        case AnalysisRunStatus::Paused:
            break;
    }
    return colors::kAccent.name();
}

QString shortDigest(const std::optional<std::string>& digest) {
    if (!digest || digest->empty()) return kNotAvailable;
    return QString::fromStdString(digest->substr(0, 16)) + QStringLiteral("…");
}

}  // namespace

AnalysisPanel::AnalysisPanel(ApplicationContext* context, QWidget* parent)
    : QWidget(parent), context_(context) {
    buildUi();
    reloadProviders();

    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(kProgressPollMs);
    connect(progressTimer_, &QTimer::timeout, this, &AnalysisPanel::applyProgressToUi);

    updateControlState();
}

AnalysisPanel::~AnalysisPanel() {
    // The worker holds raw pointers to nothing of ours, but it does write into
    // members of this object, so it must be finished before we are destroyed.
    if (cancellationToken_ != nullptr) cancellationToken_->store(true);
    if (task_ != nullptr) {
        task_->requestCancellation();
        task_->wait();
    }
}

void AnalysisPanel::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    evidenceLabel_ = new QLabel(QStringLiteral("No evidence open"), this);
    evidenceLabel_->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                      .arg(colors::kTextPrimary.name()));
    evidenceLabel_->setWordWrap(true);
    layout->addWidget(evidenceLabel_);

    // ------------------------------------------------------------ configure
    auto* configureBox = new QGroupBox(QStringLiteral("Object detection"), this);
    auto* configureLayout = new QVBoxLayout(configureBox);
    configureLayout->setSpacing(8);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);

    providerBox_ = new QComboBox(configureBox);
    providerBox_->setToolTip(
        QStringLiteral("Which detection engine runs. TRACE is not tied to any one runtime or "
                       "vendor: every provider implements the same interface."));
    form->addRow(QStringLiteral("Provider"), providerBox_);

    modelBox_ = new QComboBox(configureBox);
    modelBox_->setToolTip(QStringLiteral("Which model artefact is loaded. The SHA-256 of the exact "
                                         "file is recorded on the analysis run."));
    form->addRow(QStringLiteral("Model"), modelBox_);

    qualityBox_ = new QComboBox(configureBox);
    for (const auto quality : kQualities) {
        qualityBox_->addItem(QString::fromUtf8(toDisplayString(quality)),
                             QString::fromUtf8(toString(quality)));
    }
    qualityBox_->setCurrentIndex(1);
    qualityBox_->setToolTip(
        QStringLiteral("How densely the recording is sampled. Sampling follows the media's own "
                       "presentation timestamps, so variable frame rates are handled honestly."));
    form->addRow(QStringLiteral("Sampling"), qualityBox_);

    deviceBox_ = new QComboBox(configureBox);
    for (const auto device : kDevices) {
        deviceBox_->addItem(QString::fromUtf8(toDisplayString(device)),
                            QString::fromUtf8(toString(device)));
    }
    deviceBox_->setToolTip(
        QStringLiteral("Preferred device. What actually ran is recorded on the run — if a GPU is "
                       "unavailable the analysis falls back to CPU and says so."));
    form->addRow(QStringLiteral("Device"), deviceBox_);

    confidenceBox_ = new QDoubleSpinBox(configureBox);
    confidenceBox_->setRange(0.05, 0.99);
    confidenceBox_->setSingleStep(0.05);
    confidenceBox_->setDecimals(2);
    confidenceBox_->setValue(0.50);
    confidenceBox_->setToolTip(
        QStringLiteral("Detections scoring below this are not stored. This is the model's "
                       "confidence in a visual class — nothing more."));
    form->addRow(QStringLiteral("Minimum confidence"), confidenceBox_);

    configureLayout->addLayout(form);

    providerNotice_ = fieldLabel(configureBox, QString(), colors::kTextSecondary);
    configureLayout->addWidget(providerNotice_);

    modelStatusLabel_ = fieldLabel(configureBox, QString(), colors::kTextSecondary);
    modelStatusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    configureLayout->addWidget(modelStatusLabel_);

    auto* buttonRow = new QHBoxLayout();
    analyzeButton_ = new QPushButton(QStringLiteral("Analyze video"), configureBox);
    styleAsPrimaryAction(analyzeButton_);
    analyzeButton_->setToolTip(
        QStringLiteral("Read the managed original and store what the model reports. The evidence "
                       "file is never modified."));
    cancelButton_ = new QPushButton(QStringLiteral("Cancel analysis"), configureBox);
    cancelButton_->setEnabled(false);
    cancelButton_->setToolTip(
        QStringLiteral("Stop the run. Results already written are kept and the run is recorded as "
                       "cancelled, never as completed."));
    buttonRow->addWidget(analyzeButton_);
    buttonRow->addWidget(cancelButton_);
    buttonRow->addStretch();
    configureLayout->addLayout(buttonRow);
    layout->addWidget(configureBox);

    // ------------------------------------------------------------- progress
    auto* progressBox = new QGroupBox(QStringLiteral("Progress"), this);
    auto* progressLayout = new QVBoxLayout(progressBox);
    progressLayout->setSpacing(5);

    progressBar_ = new QProgressBar(progressBox);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(true);
    progressBar_->setFormat(QStringLiteral("%p%"));
    progressLayout->addWidget(progressBar_);

    progressHeadline_ = fieldLabel(progressBox, QStringLiteral("No analysis has been run yet."),
                                   colors::kTextSecondary);
    progressCounters_ = fieldLabel(progressBox, QString(), colors::kTextSecondary);
    progressCounters_->setFont(monospaceFont(-1));
    progressProvenance_ = fieldLabel(progressBox, QString(), colors::kTextDisabled);
    progressLayout->addWidget(progressHeadline_);
    progressLayout->addWidget(progressCounters_);
    progressLayout->addWidget(progressProvenance_);
    layout->addWidget(progressBox);

    // ----------------------------------------------------------------- runs
    auto* runBox = new QGroupBox(QStringLiteral("Analysis runs for this item"), this);
    auto* runLayout = new QVBoxLayout(runBox);
    runLayout->setSpacing(6);

    runTree_ = new QTreeWidget(runBox);
    runTree_->setColumnCount(6);
    runTree_->setHeaderLabels({QStringLiteral("Started"), QStringLiteral("Status"),
                               QStringLiteral("Model"), QStringLiteral("Device"),
                               QStringLiteral("Frames"), QStringLiteral("Detections")});
    runTree_->setRootIsDecorated(false);
    runTree_->setAlternatingRowColors(true);
    runTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    runTree_->setMinimumHeight(120);
    runTree_->header()->setStretchLastSection(false);
    runTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    runTree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    runLayout->addWidget(runTree_, 1);

    runSummary_ = fieldLabel(runBox, QStringLiteral("Select a run to see its provenance."),
                             colors::kTextSecondary);
    runSummary_->setFont(monospaceFont(-1));
    runSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    runLayout->addWidget(runSummary_);
    layout->addWidget(runBox, 1);

    connect(providerBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        reloadModels();
        updateControlState();
    });
    connect(modelBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        updateModelStatus();
        updateControlState();
    });
    connect(analyzeButton_, &QPushButton::clicked, this, &AnalysisPanel::startAnalysis);
    connect(cancelButton_, &QPushButton::clicked, this, &AnalysisPanel::cancelAnalysis);
    connect(runTree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        auto* item = runTree_->currentItem();
        if (item == nullptr) return;
        const QString runId = item->data(0, Qt::UserRole).toString();
        for (const auto& run : runs_) {
            if (QString::fromStdString(run.id) != runId) continue;
            resultsRun_ = run;
            break;
        }
        selectRunInTree(runId);
        emit resultsRunChanged();
    });
}

void AnalysisPanel::reloadProviders() {
    const QSignalBlocker blocker(providerBox_);
    providerBox_->clear();
    for (const auto& provider : DetectionProviderRegistry::instance().providers()) {
        providerBox_->addItem(QString::fromStdString(provider.displayName),
                              QString::fromStdString(provider.id));
        providerBox_->setItemData(providerBox_->count() - 1,
                                  QString::fromStdString(provider.description), Qt::ToolTipRole);
    }
    // Prefer a real detector over the mock when both are installed.
    for (int i = 0; i < providerBox_->count(); ++i) {
        if (providerBox_->itemData(i).toString() != QLatin1String("mock")) {
            providerBox_->setCurrentIndex(i);
            break;
        }
    }
    reloadModels();
}

void AnalysisPanel::reloadModels() {
    const QString providerId = providerBox_->currentData().toString();
    bool requiresModel = true;
    QString description;
    bool offDevice = false;
    for (const auto& provider : DetectionProviderRegistry::instance().providers()) {
        if (QString::fromStdString(provider.id) != providerId) continue;
        requiresModel = provider.requiresModel;
        description = QString::fromStdString(provider.description);
        offDevice = provider.sendsDataOffDevice;
        break;
    }

    {
        const QSignalBlocker blocker(modelBox_);
        modelBox_->clear();
        if (requiresModel) {
            for (const auto& descriptor : ModelManager::catalogue()) {
                modelBox_->addItem(QStringLiteral("%1 %2")
                                       .arg(QString::fromStdString(descriptor.name),
                                            QString::fromStdString(descriptor.version)),
                                   QString::fromStdString(descriptor.id));
            }
        } else {
            modelBox_->addItem(QStringLiteral("No model artefact — this provider uses none"),
                               QString());
        }
    }
    providerRequiresModel_ = requiresModel;
    modelBox_->setEnabled(requiresModel && modelBox_->count() > 0);

    QString notice = description;
    if (offDevice) {
        notice += QStringLiteral("\n⚠ This provider sends frames off this workstation.");
        providerNotice_->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px;").arg(colors::kWarning.name()));
    } else {
        providerNotice_->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    }
    providerNotice_->setText(notice);

    updateModelStatus();
}

void AnalysisPanel::updateModelStatus() {
    const QString modelId = modelBox_->currentData().toString();
    if (modelId.isEmpty()) {
        modelStatusLabel_->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
        modelStatusLabel_->setText(
            QStringLiteral("This provider runs no model artefact, so there is no model digest to "
                           "record."));
        return;
    }

    if (!context_->isInitialised()) return;

    const auto descriptor = ModelManager::describe(modelId.toStdString());
    if (!descriptor) {
        modelStatusLabel_->setText(kNotAvailable);
        return;
    }

    auto validated = context_->models().validate(*descriptor);
    if (!validated) {
        modelStatusLabel_->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px;").arg(colors::kFailure.name()));
        modelStatusLabel_->setText(QString::fromStdString(validated.error().message()));
        return;
    }
    const ModelValidation validation = validated.take();

    if (!validation.usable()) {
        modelStatusLabel_->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px;").arg(colors::kFailure.name()));
        modelStatusLabel_->setText(QString::fromStdString(validation.problem) +
                                   QStringLiteral("\nTRACE never downloads a model on its own."));
        return;
    }

    modelStatusLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    modelStatusLabel_->setText(
        QStringLiteral("Model present · %1 · SHA-256 %2\nLicence %3 · %4×%5 input · %6 classes")
            .arg(fileSize(validation.fileSize),
                 QString::fromStdString(validation.sha256.substr(0, 16)) + QStringLiteral("…"),
                 QString::fromStdString(descriptor->licence))
            .arg(descriptor->inputWidth)
            .arg(descriptor->inputHeight)
            .arg(descriptor->classes.size()));
    modelStatusLabel_->setToolTip(
        QStringLiteral("%1\nSHA-256 %2\nSource: %3")
            .arg(QString::fromStdString(validation.path.string()),
                 QString::fromStdString(validation.sha256),
                 QString::fromStdString(descriptor->source)));
}

void AnalysisPanel::setEvidence(const std::optional<Evidence>& evidence) {
    evidence_ = evidence;
    resultsRun_.reset();

    if (!evidence_) {
        evidenceLabel_->setText(QStringLiteral("No evidence open"));
        runs_.clear();
        runTree_->clear();
        if (!analysisRunning()) showIdleProgress(QStringLiteral("No analysis has been run yet."));
        updateControlState();
        emit resultsRunChanged();
        return;
    }

    evidenceLabel_->setText(QStringLiteral("%1 · %2")
                                .arg(QString::fromStdString(evidence_->evidenceNumber),
                                     QString::fromStdString(evidence_->originalFilename)));
    if (!analysisRunning()) showIdleProgress(QStringLiteral("No analysis is running."));
    refresh();
}

void AnalysisPanel::refresh() {
    reloadRuns();
    updateControlState();
}

void AnalysisPanel::reloadRuns() {
    const QString previous = resultsRunId();
    runs_.clear();
    runTree_->clear();
    // Panels can be woken by a queued signal after shutdown has released the
    // services they read through.
    if (!evidence_ || !context_->isInitialised()) {
        emit resultsRunChanged();
        return;
    }

    auto loaded = context_->analysis().runsForEvidence(evidence_->id);
    if (!loaded) {
        emit statusMessage(QStringLiteral("Analysis runs could not be read: %1")
                               .arg(QString::fromStdString(loaded.error().message())),
                           8000);
        emit resultsRunChanged();
        return;
    }
    runs_ = loaded.take();

    for (const auto& run : runs_) {
        auto* item = new QTreeWidgetItem(runTree_);
        item->setText(0, timestamp(run.startedAt.value_or(run.createdAt)));
        item->setText(1, QString::fromUtf8(toDisplayString(run.status)));
        item->setForeground(1, QBrush(QColor(statusColourName(run.status))));
        item->setText(2, QStringLiteral("%1 %2")
                             .arg(QString::fromStdString(run.modelName),
                                  QString::fromStdString(run.modelVersion)));
        item->setText(3, QString::fromStdString(run.deviceUsed.empty() ? run.deviceRequested
                                                                       : run.deviceUsed));
        item->setText(4, QString::number(run.framesAnalyzed));
        item->setText(5, QString::number(run.detectionsStored));
        item->setData(0, Qt::UserRole, QString::fromStdString(run.id));
        if (run.errorMessage) {
            item->setToolTip(1, QString::fromStdString(*run.errorMessage));
        }
    }

    // Keep viewing the same run across a refresh when it still exists;
    // otherwise fall back to the most recent run with usable results.
    std::optional<AnalysisRun> chosen;
    for (const auto& run : runs_) {
        if (QString::fromStdString(run.id) == previous) chosen = run;
    }
    if (!chosen) {
        auto latest = context_->analysis().latestUsableRun(evidence_->id);
        if (latest && latest.value().has_value()) chosen = *latest.take();
    }
    resultsRun_ = chosen;
    selectRunInTree(resultsRunId());
    emit resultsRunChanged();
}

void AnalysisPanel::selectRunInTree(const QString& runId) {
    for (int i = 0; i < runTree_->topLevelItemCount(); ++i) {
        auto* item = runTree_->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() != runId) continue;
        const QSignalBlocker blocker(runTree_);
        runTree_->setCurrentItem(item);
        break;
    }

    if (!resultsRun_) {
        runSummary_->setText(runs_.empty()
                                 ? QStringLiteral("This item has not been analysed.")
                                 : QStringLiteral("No run with usable results is selected."));
        return;
    }

    const AnalysisRun& run = *resultsRun_;
    QString text =
        QStringLiteral("Run          %1\nProvider     %2 %3\nRuntime      %4\n")
            .arg(QString::fromStdString(run.id.substr(0, 8)),
                 QString::fromStdString(run.providerName),
                 QString::fromStdString(run.providerVersion),
                 QString::fromStdString(run.runtime));
    text += QStringLiteral("Model        %1 %2\nModel SHA    %3\n")
                .arg(QString::fromStdString(run.modelName),
                     QString::fromStdString(run.modelVersion), shortDigest(run.modelSha256));
    text += QStringLiteral("Device       requested %1, used %2\nEvidence SHA %3\n")
                .arg(QString::fromStdString(run.deviceRequested),
                     QString::fromStdString(run.deviceUsed.empty() ? "—" : run.deviceUsed),
                     QString::fromStdString(run.evidenceSha256.substr(0, 16)) +
                         QStringLiteral("…"));
    text += QStringLiteral("Frames       %1 analysed · %2 detections stored\nStatus       %3")
                .arg(run.framesAnalyzed)
                .arg(run.detectionsStored)
                .arg(QString::fromUtf8(toDisplayString(run.status)));
    if (run.errorMessage) {
        text += QStringLiteral("\nError        %1").arg(QString::fromStdString(*run.errorMessage));
    }
    if (!producedCompleteResults(run.status)) {
        text += QStringLiteral(
            "\n\nThese results do not cover the whole recording. They are shown as recorded.");
    }
    runSummary_->setText(text);
    runSummary_->setToolTip(
        QStringLiteral("Model SHA-256 %1\nEvidence SHA-256 %2\nConfiguration %3")
            .arg(QString::fromStdString(run.modelSha256.value_or("not applicable")),
                 QString::fromStdString(run.evidenceSha256),
                 QString::fromStdString(run.configurationJson)));
}

QString AnalysisPanel::resultsRunId() const {
    return resultsRun_ ? QString::fromStdString(resultsRun_->id) : QString();
}

bool AnalysisPanel::analysisRunning() const { return runInFlight_; }

void AnalysisPanel::updateControlState() {
    const bool busy = analysisRunning();
    const bool haveEvidence = evidence_.has_value() && context_->currentCase().has_value();
    const bool isVideo = haveEvidence && evidence_->mediaType == MediaType::Video;
    const bool haveProvider = providerBox_->count() > 0;

    analyzeButton_->setEnabled(!busy && isVideo && haveProvider);
    cancelButton_->setEnabled(busy);
    providerBox_->setEnabled(!busy);
    modelBox_->setEnabled(!busy && providerRequiresModel_ && modelBox_->count() > 0);
    qualityBox_->setEnabled(!busy);
    deviceBox_->setEnabled(!busy);
    confidenceBox_->setEnabled(!busy);
    runTree_->setEnabled(!busy);

    if (!haveEvidence) {
        analyzeButton_->setToolTip(QStringLiteral("Open an evidence item first."));
    } else if (!isVideo) {
        analyzeButton_->setToolTip(
            QStringLiteral("Object detection runs on video items. This item is %1.")
                .arg(QString::fromUtf8(toDisplayString(evidence_->mediaType)).toLower()));
    } else {
        analyzeButton_->setToolTip(
            QStringLiteral("Read the managed original and store what the model reports. The "
                           "evidence file is never modified."));
    }
}

DetectionAnalysisRequest AnalysisPanel::buildRequest() const {
    DetectionAnalysisRequest request;
    request.caseId = context_->currentCase()->id;
    request.caseNumber = context_->currentCase()->caseNumber;
    request.evidenceId = evidence_->id;
    request.evidenceNumber = evidence_->evidenceNumber;
    request.mediaPath = context_->evidence().absolutePath(*evidence_);
    request.evidenceSha256 = evidence_->sha256;
    request.hardwareDevice = context_->decodeDevice();
    request.providerId = providerBox_->currentData().toString().toStdString();
    request.modelId = modelBox_->currentData().toString().toStdString();
    request.device = devicePreferenceFromString(deviceBox_->currentData().toString().toStdString());
    request.quality =
        analysisQualityFromString(qualityBox_->currentData().toString().toStdString());
    request.confidenceThreshold = confidenceBox_->value();
    return request;
}

void AnalysisPanel::startAnalysis() {
    if (analysisRunning() || !context_->isInitialised()) return;
    if (!evidence_ || !context_->currentCase()) {
        emit statusMessage(QStringLiteral("Open an evidence item before running an analysis."),
                           5000);
        return;
    }
    if (evidence_->mediaType != MediaType::Video) {
        emit statusMessage(QStringLiteral("Object detection runs on video items."), 5000);
        return;
    }
    if (providerBox_->count() == 0) {
        emit statusMessage(QStringLiteral("No detection provider is installed in this build."),
                           8000);
        return;
    }

    DetectionAnalysisRequest request = buildRequest();

    // The key lives here rather than inside the request, because the pipeline
    // borrows it for the length of the run and the run outlives buildRequest().
    auto caseKey = context_->evidence().caseKey(request.caseId);
    if (!caseKey) {
        emit statusMessage(QString::fromStdString(caseKey.error().message()), 8000);
        return;
    }
    analysisKey_ = std::make_shared<CaseKeyHandle>(caseKey.take());
    request.mediaKey = analysisKey_->get();

    cancellationToken_ = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        latestProgress_ = AnalysisProgress{};
        lastOutcome_.reset();
    }

    progressBar_->setValue(0);
    progressHeadline_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kAccent.name()));
    progressHeadline_->setText(QStringLiteral("Loading model and opening media…"));
    progressCounters_->setText(QString());
    progressProvenance_->setText(QString());

    runningEvidenceNumber_ = QString::fromStdString(evidence_->evidenceNumber);
    runInFlight_ = true;
    task_ = std::make_unique<BackgroundTask>();
    connect(task_.get(), &BackgroundTask::finished, this,
            [this](bool succeeded, const QString& message) {
                runInFlight_ = false;
                if (task_ != nullptr) {
                    task_->wait();
                    task_.reset();
                }
                progressTimer_->stop();
                cancellationToken_.reset();

                std::optional<AnalysisOutcome> outcome;
                {
                    std::lock_guard<std::mutex> lock(progressMutex_);
                    outcome = lastOutcome_;
                }

                if (outcome) {
                    progressBar_->setValue(producedCompleteResults(outcome->status) ? 1000
                                                                                    : progressBar_->value());
                    progressProvenance_->setText(
                        QStringLiteral("Device %1 · %2 frames · %3 detections · %4 s")
                            .arg(QString::fromStdString(outcome->deviceUsed))
                            .arg(outcome->framesAnalyzed)
                            .arg(outcome->detectionsStored)
                            .arg(outcome->elapsedSeconds, 0, 'f', 1));
                }
                progressHeadline_->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                        .arg((succeeded ? colors::kVerified : colors::kFailure).name()));
                progressHeadline_->setText(message);

                context_->notifyAnalysisRunsChanged();
                context_->notifyDetectionsChanged();
                context_->notifyAuditChanged();
                reloadRuns();
                updateControlState();
                emit statusMessage(message, 10000);
                emit analysisFinished(
                    succeeded, message,
                    static_cast<int>(outcome ? outcome->status : AnalysisRunStatus::Failed));
            });

    auto pipeline = context_->makeAnalysisPipeline();
    auto token = cancellationToken_;
    DetectionAnalysisRequest job = request;
    job.cancellationToken = token;

    progressTimer_->start();

    // Captured so the key outlives this scope: the worker decodes for as long
    // as the run takes, and a dangling key pointer would be a crash rather than
    // a failed decode.
    task_->start([this, pipeline, job, key = analysisKey_](BackgroundTask& task) mutable {
        const auto callback = [this, &task](const AnalysisProgress& progress) {
            {
                std::lock_guard<std::mutex> lock(progressMutex_);
                latestProgress_ = progress;
            }
            return !task.cancellationRequested();
        };

        auto executed = pipeline.execute(job, callback);
        if (!executed) {
            task.reportFinished(false, QStringLiteral("Analysis failed: %1")
                                           .arg(QString::fromStdString(executed.error().message())));
            return;
        }

        const AnalysisOutcome outcome = executed.take();
        {
            std::lock_guard<std::mutex> lock(progressMutex_);
            lastOutcome_ = outcome;
        }

        QString message;
        switch (outcome.status) {
            case AnalysisRunStatus::Completed:
                message = QStringLiteral("Analysis complete — %1 detections in %2 frames (%3)")
                              .arg(outcome.detectionsStored)
                              .arg(outcome.framesAnalyzed)
                              .arg(QString::fromStdString(outcome.deviceUsed));
                break;
            case AnalysisRunStatus::Cancelled:
                message = QStringLiteral("Analysis cancelled — %1 detections from %2 frames kept")
                              .arg(outcome.detectionsStored)
                              .arg(outcome.framesAnalyzed);
                break;
            case AnalysisRunStatus::Partial:
                message = QStringLiteral("Analysis stopped early — partial results kept (%1)")
                              .arg(QString::fromStdString(outcome.error.value_or("no reason recorded")));
                break;
            default:
                message = QStringLiteral("Analysis failed: %1")
                              .arg(QString::fromStdString(outcome.error.value_or("no reason recorded")));
                break;
        }
        task.reportFinished(outcome.status == AnalysisRunStatus::Completed, message);
    });

    updateControlState();
    emit analysisStarted();
    emit statusMessage(QStringLiteral("Analysing %1…").arg(runningEvidenceNumber_), 0);
}

void AnalysisPanel::cancelAnalysis() {
    if (!analysisRunning() || task_ == nullptr) return;
    if (cancellationToken_ != nullptr) cancellationToken_->store(true);
    task_->requestCancellation();
    cancelButton_->setEnabled(false);
    progressHeadline_->setText(QStringLiteral("Cancelling — results already written are kept…"));
}

void AnalysisPanel::applyProgressToUi() {
    AnalysisProgress progress;
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        progress = latestProgress_;
    }
    if (progress.framesAnalyzed == 0 && progress.elapsedSeconds == 0.0) return;

    progressBar_->setValue(static_cast<int>(progress.fractionComplete * 1000.0));
    progressHeadline_->setText(
        QStringLiteral("Analysing %1 — %2 of %3 (%4%)")
            .arg(runningEvidenceNumber_, timecode(progress.positionUs),
                 timecode(progress.durationUs))
            .arg(progress.fractionComplete * 100.0, 0, 'f', 1));
    progressCounters_->setText(
        QStringLiteral("frames %1   detections %2   %3 fps   elapsed %4 s")
            .arg(progress.framesAnalyzed)
            .arg(progress.detectionsFound)
            .arg(progress.framesPerSecond, 0, 'f', 1)
            .arg(progress.elapsedSeconds, 0, 'f', 1));
    progressProvenance_->setText(QStringLiteral("device %1 · model %2")
                                     .arg(QString::fromStdString(progress.device),
                                          QString::fromStdString(progress.model)));
    emit progressUpdated(progress.framesAnalyzed, progress.detectionsFound,
                         progress.fractionComplete);
}

void AnalysisPanel::showIdleProgress(const QString& message) {
    progressBar_->setValue(0);
    progressHeadline_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    progressHeadline_->setText(message);
    progressCounters_->setText(QString());
    progressProvenance_->setText(QString());
}

}  // namespace trace::ui
