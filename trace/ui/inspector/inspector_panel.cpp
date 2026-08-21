#include "ui/inspector/inspector_panel.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "core/common/time_utils.h"
#include "ui/app/application_context.h"
#include "ui/common/background_task.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"

namespace trace::ui {
namespace {

QLabel* valueLabel(QWidget* parent, bool monospace = false) {
    auto* label = new QLabel(kNotAvailable, parent);
    label->setWordWrap(true);
    label->setMinimumWidth(1);  // allow the panel to shrink; text wraps instead
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (monospace) label->setFont(monospaceFont(-1));
    return label;
}

void setRowValue(QFormLayout* form, int row, const QString& value) {
    if (row >= form->rowCount()) return;
    auto* item = form->itemAt(row, QFormLayout::FieldRole);
    if (item == nullptr) return;
    if (auto* label = qobject_cast<QLabel*>(item->widget()); label != nullptr) {
        label->setText(value.isEmpty() ? kNotAvailable : value);
    }
}

QGroupBox* section(const QString& title, QWidget* parent) {
    auto* box = new QGroupBox(title, parent);
    return box;
}

/// Fixed-width caption so the value column keeps a predictable size and long
/// paths wrap instead of squeezing short values onto two lines.
QLabel* captionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFixedWidth(118);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return label;
}

}  // namespace

InspectorPanel::InspectorPanel(ApplicationContext* context, QWidget* parent)
    : QWidget(parent), context_(context) {
    buildUi();
    connect(context_, &ApplicationContext::currentEvidenceChanged, this,
            [this] { showEvidence(context_->currentEvidence()); });
    connect(context_, &ApplicationContext::derivedAssetsChanged, this,
            &InspectorPanel::updateDerivedAssets);
    showEvidence(std::nullopt);
}

InspectorPanel::~InspectorPanel() {
    if (verifyTask_ != nullptr) {
        verifyTask_->requestCancellation();
        verifyTask_->wait();
    }
}

void InspectorPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);

    evidenceNumber_ = new QLabel(QStringLiteral("No evidence selected"), content);
    evidenceNumber_->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600; color: %1;")
                                       .arg(colors::kTextPrimary.name()));
    evidenceFilename_ = new QLabel(content);
    evidenceFilename_->setWordWrap(true);
    evidenceFilename_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;").arg(colors::kTextSecondary.name()));
    layout->addWidget(evidenceNumber_);
    layout->addWidget(evidenceFilename_);

    // ------------------------------------------------------------- integrity
    auto* integrityBox = section(QStringLiteral("INTEGRITY"), content);
    auto* integrityLayout = new QVBoxLayout(integrityBox);
    integrityLayout->setSpacing(8);

    integrityBadge_ = new QLabel(QStringLiteral("NOT VERIFIED"), integrityBox);
    integrityBadge_->setStyleSheet(badgeStyleSheet(colors::kNeutralBadge));
    integrityBadge_->setAlignment(Qt::AlignCenter);
    integrityBadge_->setFixedWidth(160);
    integrityLayout->addWidget(integrityBadge_);

    auto* hashCaption = new QLabel(QStringLiteral("SHA-256 recorded at ingestion"), integrityBox);
    hashCaption->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    integrityLayout->addWidget(hashCaption);

    hashLabel_ = valueLabel(integrityBox, true);
    hashLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(colors::kTextPrimary.name()));
    integrityLayout->addWidget(hashLabel_);

    lastVerifiedLabel_ = new QLabel(integrityBox);
    lastVerifiedLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    integrityLayout->addWidget(lastVerifiedLabel_);

    integrityDetail_ = new QLabel(integrityBox);
    integrityDetail_->setWordWrap(true);
    integrityDetail_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    integrityLayout->addWidget(integrityDetail_);

    verifyProgress_ = new QProgressBar(integrityBox);
    verifyProgress_->setRange(0, 100);
    verifyProgress_->setVisible(false);
    integrityLayout->addWidget(verifyProgress_);

    verifyButton_ = new QPushButton(QStringLiteral("Verify integrity"), integrityBox);
    verifyButton_->setToolTip(
        QStringLiteral("Re-read the managed original and compare its SHA-256 with the digest "
                       "recorded at ingestion"));
    verifyButton_->setEnabled(false);
    integrityLayout->addWidget(verifyButton_);
    layout->addWidget(integrityBox);
    connect(verifyButton_, &QPushButton::clicked, this, &InspectorPanel::verifyIntegrity);

    // -------------------------------------------------------------- evidence
    auto* evidenceBox = section(QStringLiteral("EVIDENCE"), content);
    evidenceForm_ = new QFormLayout(evidenceBox);
    evidenceForm_->setSpacing(6);
    evidenceForm_->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    evidenceForm_->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    for (const auto& caption :
         {QStringLiteral("Media type"), QStringLiteral("Media status"), QStringLiteral("File size"),
          QStringLiteral("Imported"), QStringLiteral("Imported by"),
          QStringLiteral("Source path"), QStringLiteral("Source modified"),
          QStringLiteral("Managed original"), QStringLiteral("Evidence UUID")}) {
        evidenceForm_->addRow(captionLabel(caption, evidenceBox),
                              valueLabel(evidenceBox, caption.contains("path") ||
                                                          caption.contains("UUID")));
    }
    layout->addWidget(evidenceBox);

    // ----------------------------------------------------------------- media
    auto* mediaBox = section(QStringLiteral("TECHNICAL METADATA"), content);
    mediaForm_ = new QFormLayout(mediaBox);
    mediaForm_->setSpacing(6);
    mediaForm_->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    mediaForm_->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    for (const auto& caption :
         {QStringLiteral("Container"), QStringLiteral("Duration"), QStringLiteral("Overall bitrate"),
          QStringLiteral("Video codec"), QStringLiteral("Profile"), QStringLiteral("Resolution"),
          QStringLiteral("Display aspect"), QStringLiteral("Pixel format"),
          QStringLiteral("Average frame rate"), QStringLiteral("Nominal frame rate"),
          QStringLiteral("Frame rate mode"), QStringLiteral("Video time base"),
          QStringLiteral("Frame count"), QStringLiteral("Audio codec"),
          QStringLiteral("Audio channels"), QStringLiteral("Sample rate"),
          QStringLiteral("Audio bitrate"), QStringLiteral("Creation time"),
          QStringLiteral("Location"), QStringLiteral("Recording device"),
          QStringLiteral("Extraction")}) {
        mediaForm_->addRow(captionLabel(caption, mediaBox), valueLabel(mediaBox));
    }
    layout->addWidget(mediaBox);

    // --------------------------------------------------------------- streams
    auto* streamBox = section(QStringLiteral("STREAMS"), content);
    auto* streamLayout = new QVBoxLayout(streamBox);
    streamTree_ = new QTreeWidget(streamBox);
    streamTree_->setColumnCount(3);
    streamTree_->setHeaderLabels(
        {QStringLiteral("#"), QStringLiteral("Type"), QStringLiteral("Details")});
    streamTree_->setRootIsDecorated(false);
    streamTree_->setMinimumHeight(90);
    streamTree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    streamLayout->addWidget(streamTree_);
    layout->addWidget(streamBox);

    // -------------------------------------------------------- derived assets
    auto* derivedBox = section(QStringLiteral("DERIVED ASSETS"), content);
    auto* derivedLayout = new QVBoxLayout(derivedBox);
    auto* derivedCaption =
        new QLabel(QStringLiteral("Everything produced from this original, with its own digest."),
                   derivedBox);
    derivedCaption->setWordWrap(true);
    derivedCaption->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    derivedLayout->addWidget(derivedCaption);
    derivedTree_ = new QTreeWidget(derivedBox);
    derivedTree_->setColumnCount(3);
    derivedTree_->setHeaderLabels(
        {QStringLiteral("Type"), QStringLiteral("Source time"), QStringLiteral("File")});
    derivedTree_->setRootIsDecorated(false);
    derivedTree_->setMinimumHeight(90);
    derivedTree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    derivedLayout->addWidget(derivedTree_);
    layout->addWidget(derivedBox);

    // --------------------------------------------------------- raw metadata
    auto* rawBox = section(QStringLiteral("RAW CONTAINER METADATA"), content);
    auto* rawLayout = new QVBoxLayout(rawBox);
    rawToggle_ = new QPushButton(QStringLiteral("Show raw metadata"), rawBox);
    rawLayout->addWidget(rawToggle_);
    rawMetadata_ = new QPlainTextEdit(rawBox);
    rawMetadata_->setReadOnly(true);
    rawMetadata_->setFont(monospaceFont(-1));
    rawMetadata_->setMinimumHeight(220);
    rawMetadata_->setVisible(false);
    rawLayout->addWidget(rawMetadata_);
    layout->addWidget(rawBox);
    connect(rawToggle_, &QPushButton::clicked, this, [this] {
        const bool visible = !rawMetadata_->isVisible();
        rawMetadata_->setVisible(visible);
        rawToggle_->setText(visible ? QStringLiteral("Hide raw metadata")
                                    : QStringLiteral("Show raw metadata"));
    });

    layout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);
    setMinimumWidth(390);
}

void InspectorPanel::showEvidence(const std::optional<Evidence>& evidence) {
    evidence_ = evidence;
    metadata_.reset();
    if (evidence_) {
        auto found = context_->evidence().metadataFor(evidence_->id);
        if (found && found.value().has_value()) metadata_ = *found.take();
    }
    refresh();
}

void InspectorPanel::refresh() {
    if (!context_->isInitialised()) return;
    const bool has = evidence_.has_value();
    verifyButton_->setEnabled(has);

    if (!has) {
        evidenceNumber_->setText(QStringLiteral("No evidence selected"));
        evidenceFilename_->clear();
        integrityBadge_->setText(QStringLiteral("—"));
        integrityBadge_->setStyleSheet(badgeStyleSheet(colors::kNeutralBadge));
        hashLabel_->setText(kNotAvailable);
        lastVerifiedLabel_->clear();
        integrityDetail_->clear();
        for (int row = 0; row < evidenceForm_->rowCount(); ++row) {
            setRowValue(evidenceForm_, row, kNotAvailable);
        }
        for (int row = 0; row < mediaForm_->rowCount(); ++row) {
            setRowValue(mediaForm_, row, kNotAvailable);
        }
        streamTree_->clear();
        derivedTree_->clear();
        rawMetadata_->clear();
        return;
    }

    evidenceNumber_->setText(QString::fromStdString(evidence_->evidenceNumber));
    evidenceFilename_->setText(QString::fromStdString(evidence_->originalFilename));

    int row = 0;
    setRowValue(evidenceForm_, row++, QString::fromUtf8(toDisplayString(evidence_->mediaType)));
    setRowValue(evidenceForm_, row++, QString::fromUtf8(toDisplayString(evidence_->mediaStatus)));
    setRowValue(evidenceForm_, row++, fileSize(evidence_->fileSize));
    setRowValue(evidenceForm_, row++, timestamp(evidence_->ingestedAt));
    setRowValue(evidenceForm_, row++, text(evidence_->ingestedBy));
    setRowValue(evidenceForm_, row++, text(evidence_->sourcePath));
    setRowValue(evidenceForm_, row++, optionalTimestamp(evidence_->sourceModifiedAt));
    setRowValue(evidenceForm_, row++, text(evidence_->storageRelPath));
    setRowValue(evidenceForm_, row++, text(evidence_->id));

    updateIntegritySection();
    updateMetadataSection();
    updateDerivedAssets();
}

void InspectorPanel::updateIntegritySection() {
    if (!evidence_) return;
    integrityBadge_->setText(QString::fromUtf8(toDisplayString(evidence_->integrityStatus)));
    integrityBadge_->setStyleSheet(badgeStyleSheet(integrityColour(evidence_->integrityStatus)));
    hashLabel_->setText(formatDigestForDisplay(evidence_->sha256));
    lastVerifiedLabel_->setText(
        QStringLiteral("Last checked: %1").arg(optionalTimestamp(evidence_->lastVerifiedAt)));

    switch (evidence_->integrityStatus) {
        case IntegrityStatus::Verified:
            integrityDetail_->setText(QStringLiteral(
                "The managed original matches the digest recorded at ingestion."));
            break;
        case IntegrityStatus::Failed:
            integrityDetail_->setText(QStringLiteral(
                "The managed original no longer matches the digest recorded at ingestion. The "
                "recorded digest has not been changed. Review the audit trail before proceeding."));
            break;
        case IntegrityStatus::Error:
            integrityDetail_->setText(
                QStringLiteral("The last check could not complete. See the audit trail."));
            break;
        case IntegrityStatus::NotVerified:
        case IntegrityStatus::Verifying:
            integrityDetail_->setText(
                QStringLiteral("Run a verification to re-read the file and compare digests."));
            break;
    }
}

void InspectorPanel::updateMetadataSection() {
    int row = 0;
    if (!metadata_) {
        for (int i = 0; i < mediaForm_->rowCount(); ++i) setRowValue(mediaForm_, i, kNotAvailable);
        setRowValue(mediaForm_, mediaForm_->rowCount() - 1,
                    QStringLiteral("No metadata stored for this item"));
        streamTree_->clear();
        rawMetadata_->clear();
        return;
    }

    const MediaMetadata& m = *metadata_;
    setRowValue(mediaForm_, row++, optionalText(m.formatLongName).replace(
                                       kNotAvailable, optionalText(m.containerFormat)));
    setRowValue(mediaForm_, row++,
                m.durationUs ? QStringLiteral("%1  (%2)").arg(
                                   timecode(*m.durationUs),
                                   QString::fromStdString(formatDurationHuman(*m.durationUs)))
                             : kNotAvailable);
    setRowValue(mediaForm_, row++, optionalNumber(m.bitRate, QStringLiteral("bit/s")));
    setRowValue(mediaForm_, row++, optionalText(m.videoCodec));
    setRowValue(mediaForm_, row++, optionalText(m.videoProfile));
    setRowValue(mediaForm_, row++, resolution(m.width, m.height));
    setRowValue(mediaForm_, row++, optionalText(m.displayAspect));
    setRowValue(mediaForm_, row++, optionalText(m.pixelFormat));
    setRowValue(mediaForm_, row++, optionalDecimal(m.averageFrameRate, 3, QStringLiteral("fps")));
    setRowValue(mediaForm_, row++, optionalDecimal(m.nominalFrameRate, 3, QStringLiteral("fps")));
    setRowValue(mediaForm_, row++, QString::fromUtf8(toDisplayString(m.frameRateMode)));
    setRowValue(mediaForm_, row++, optionalText(m.videoTimeBase));
    setRowValue(mediaForm_, row++, optionalNumber(m.frameCount));
    setRowValue(mediaForm_, row++, optionalText(m.audioCodec));
    setRowValue(mediaForm_, row++,
                m.audioChannels
                    ? QStringLiteral("%1 (%2)").arg(optionalNumber(m.audioChannels),
                                                    optionalText(m.audioChannelLayout))
                    : kNotAvailable);
    setRowValue(mediaForm_, row++, optionalNumber(m.audioSampleRate, QStringLiteral("Hz")));
    setRowValue(mediaForm_, row++, optionalNumber(m.audioBitRate, QStringLiteral("bit/s")));
    setRowValue(mediaForm_, row++, optionalTimestamp(m.creationTime));
    setRowValue(mediaForm_, row++, optionalText(m.gpsLocation));
    setRowValue(mediaForm_, row++,
                m.make || m.model
                    ? QStringLiteral("%1 %2").arg(optionalText(m.make), optionalText(m.model)).trimmed()
                    : kNotAvailable);
    setRowValue(mediaForm_, row++,
                QStringLiteral("%1 — %2")
                    .arg(QString::fromUtf8(toDisplayString(m.extractionStatus)),
                         text(m.extractor)));

    streamTree_->clear();
    for (const auto& stream : m.streams) {
        auto* item = new QTreeWidgetItem(streamTree_);
        item->setText(0, QString::number(stream.index));
        item->setText(1, QString::fromUtf8(toString(stream.type)));
        QStringList details;
        details << optionalText(stream.codecName);
        if (stream.width && stream.height) {
            details << resolution(stream.width, stream.height);
        }
        if (stream.frameRate) details << optionalDecimal(stream.frameRate, 3, QStringLiteral("fps"));
        if (stream.sampleRate) details << optionalNumber(stream.sampleRate, QStringLiteral("Hz"));
        if (stream.channels) details << optionalNumber(stream.channels, QStringLiteral("ch"));
        if (stream.language) details << optionalText(stream.language);
        item->setText(2, details.join(QStringLiteral("  ·  ")));
    }
    streamTree_->resizeColumnToContents(0);
    streamTree_->resizeColumnToContents(1);

    rawMetadata_->setPlainText(QString::fromStdString(m.rawMetadataJson));
}

void InspectorPanel::updateDerivedAssets() {
    derivedTree_->clear();
    if (!evidence_ || !context_->isInitialised()) return;

    auto assets = context_->derivedAssets().listForEvidence(evidence_->id);
    if (!assets) return;
    for (const auto& asset : assets.value()) {
        auto* item = new QTreeWidgetItem(derivedTree_);
        item->setText(0, QString::fromUtf8(toDisplayString(asset.type)));
        item->setText(1, asset.sourceStartUs ? timecode(*asset.sourceStartUs) : kNotAvailable);
        item->setText(2, QString::fromStdString(asset.filename));
        item->setToolTip(2, QStringLiteral("%1\nSHA-256 %2\nCreated %3 by %4")
                                .arg(QString::fromStdString(asset.storageRelPath),
                                     QString::fromStdString(asset.sha256.value_or("n/a")),
                                     timestamp(asset.createdAt),
                                     QString::fromStdString(asset.createdBy)));
    }
    derivedTree_->resizeColumnToContents(0);
    derivedTree_->resizeColumnToContents(1);
}

void InspectorPanel::verifyIntegrity() {
    if (!evidence_ || verifyTask_ != nullptr) return;

    verifyButton_->setEnabled(false);
    verifyProgress_->setValue(0);
    verifyProgress_->setVisible(true);
    integrityBadge_->setText(QStringLiteral("VERIFYING"));
    integrityBadge_->setStyleSheet(badgeStyleSheet(colors::kAccent));
    emit statusMessage(QStringLiteral("Verifying %1…")
                           .arg(QString::fromStdString(evidence_->evidenceNumber)),
                       0);

    verifyTask_ = std::make_unique<BackgroundTask>();
    const Evidence evidence = *evidence_;
    const QString caseNumber = context_->currentCaseNumber();

    connect(verifyTask_.get(), &BackgroundTask::progressChanged, this,
            [this](int percent, const QString&, const QString&) { verifyProgress_->setValue(percent); });

    connect(verifyTask_.get(), &BackgroundTask::finished, this,
            [this, evidence](bool verified, const QString& message) {
                verifyProgress_->setVisible(false);
                verifyButton_->setEnabled(true);
                if (verifyTask_ != nullptr) {
                    verifyTask_->wait();
                    verifyTask_.reset();
                }
                context_->refreshCurrentEvidence();
                context_->notifyEvidenceChanged();
                context_->notifyAuditChanged();

                auto reloaded = context_->evidence().findById(evidence.id);
                if (reloaded && reloaded.value().has_value()) evidence_ = *reloaded.take();
                updateIntegritySection();

                emit integrityChecked(verified);
                emit statusMessage(message, 8000);

                if (verified) {
                    QMessageBox::information(this, QStringLiteral("Integrity verified"), message);
                } else {
                    QMessageBox::warning(this, QStringLiteral("Integrity verification"), message);
                }
            });

    auto* context = context_;
    verifyTask_->start([context, evidence, caseNumber](BackgroundTask& task) {
        auto result = context->integrity().verify(
            evidence, caseNumber.toStdString(), [&task](const HashProgress& progress) {
                if (progress.totalBytes > 0) {
                    task.reportProgress(static_cast<int>(progress.bytesProcessed * 100 /
                                                         progress.totalBytes),
                                        QString(), QString());
                }
                return !task.cancellationRequested();
            });

        if (!result) {
            task.reportFinished(false, QStringLiteral("Verification could not complete: %1")
                                           .arg(QString::fromStdString(result.error().toString())));
            return;
        }
        const IntegrityCheck check = result.take();
        if (check.verified) {
            task.reportFinished(
                true, QStringLiteral("VERIFIED\n\n%1 matches the SHA-256 recorded at ingestion.\n\n%2")
                          .arg(QString::fromStdString(evidence.evidenceNumber),
                               QString::fromStdString(check.computedSha256)));
        } else {
            task.reportFinished(
                false,
                QStringLiteral(
                    "INTEGRITY FAILURE\n\n%1 no longer matches the SHA-256 recorded at "
                    "ingestion.\n\nRecorded: %2\nComputed: %3\n\nThe recorded digest has not been "
                    "changed. This result is in the audit trail.")
                    .arg(QString::fromStdString(evidence.evidenceNumber),
                         QString::fromStdString(check.storedSha256),
                         QString::fromStdString(check.computedSha256)));
        }
    });
}

}  // namespace trace::ui
