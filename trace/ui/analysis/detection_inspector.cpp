#include "ui/analysis/detection_inspector.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include "ui/common/detection_overlay.h"
#include "ui/common/display_utils.h"
#include "ui/common/theme.h"

namespace trace::ui {
namespace {

enum ObservationRow { kClassRow = 0, kGroupRow, kConfidenceRow, kTimestampRow, kFrameRow };
enum GeometryRow { kNormalisedRow = 0, kPixelRow, kSourceSizeRow };
enum ReviewRow { kStateRow = 0, kReviewMethodRow, kReviewedByRow, kReviewedAtRow, kNoteRow };
enum ProvenanceRow {
    kEvidenceRow = 0,
    kEvidenceDigestRow,
    kProviderRow,
    kRuntimeRow,
    kModelRow,
    kModelDigestRow,
    kDeviceRow,
    kSamplingRow,
    kRunRow,
    kRunStatusRow,
    kRunStartedRow,
    kDetectionIdRow
};

QLabel* valueLabel(QWidget* parent, bool monospace = false) {
    auto* label = new QLabel(kNotAvailable, parent);
    label->setWordWrap(true);
    label->setMinimumWidth(1);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (monospace) label->setFont(monospaceFont(-1));
    return label;
}

QLabel* captionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFixedWidth(118);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
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

QFormLayout* buildSection(QVBoxLayout* parentLayout, QWidget* parent, const QString& title,
                          const std::vector<std::pair<QString, bool>>& rows) {
    auto* box = new QGroupBox(title, parent);
    auto* form = new QFormLayout(box);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(4);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    for (const auto& [caption, monospace] : rows) {
        form->addRow(captionLabel(caption, box), valueLabel(box, monospace));
    }
    parentLayout->addWidget(box);
    return form;
}

QString boxText(const NormalizedBox& box) {
    return QStringLiteral("x %1  y %2  w %3  h %4")
        .arg(box.x, 0, 'f', 4)
        .arg(box.y, 0, 'f', 4)
        .arg(box.width, 0, 'f', 4)
        .arg(box.height, 0, 'f', 4);
}

}  // namespace

DetectionInspector::DetectionInspector(QWidget* parent) : QWidget(parent) { buildUi(); }

void DetectionInspector::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    headline_ = new QLabel(QString(), content);
    headline_->setWordWrap(true);
    headline_->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; font-weight: 600;")
                                 .arg(colors::kTextPrimary.name()));
    layout->addWidget(headline_);

    placeholder_ = new QLabel(
        QStringLiteral("Select a detection in the detections list, or click a box on the video, to "
                       "see where it came from."),
        content);
    placeholder_->setWordWrap(true);
    placeholder_->setStyleSheet(
        QStringLiteral("color: %1;").arg(colors::kTextSecondary.name()));
    layout->addWidget(placeholder_);

    body_ = new QWidget(content);
    auto* bodyLayout = new QVBoxLayout(body_);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(10);

    observationForm_ = buildSection(bodyLayout, body_, QStringLiteral("Observation"),
                                    {{QStringLiteral("Class"), false},
                                     {QStringLiteral("Group"), false},
                                     {QStringLiteral("Confidence"), false},
                                     {QStringLiteral("Timestamp"), true},
                                     {QStringLiteral("Frame"), true}});

    geometryForm_ = buildSection(bodyLayout, body_, QStringLiteral("Location in frame"),
                                 {{QStringLiteral("Normalised"), true},
                                  {QStringLiteral("Pixels"), true},
                                  {QStringLiteral("Source frame"), true}});

    reviewForm_ = buildSection(bodyLayout, body_, QStringLiteral("Human review"),
                               {{QStringLiteral("State"), false},
                                {QStringLiteral("Reviewed by"), false},
                                {QStringLiteral("Reviewed at"), false},
                                {QStringLiteral("Note"), false}});

    provenanceForm_ = buildSection(bodyLayout, body_, QStringLiteral("Provenance"),
                                   {{QStringLiteral("Evidence"), false},
                                    {QStringLiteral("Evidence SHA-256"), true},
                                    {QStringLiteral("Provider"), false},
                                    {QStringLiteral("Runtime"), false},
                                    {QStringLiteral("Model"), false},
                                    {QStringLiteral("Model SHA-256"), true},
                                    {QStringLiteral("Device used"), false},
                                    {QStringLiteral("Sampling"), false},
                                    {QStringLiteral("Analysis run"), true},
                                    {QStringLiteral("Run status"), false},
                                    {QStringLiteral("Run started"), false},
                                    {QStringLiteral("Detection id"), true}});

    auto* caution = new QLabel(
        QStringLiteral("TRACE reports a visual class and the model's confidence in it. It does not "
                       "identify individuals, read number plates, or draw conclusions about what "
                       "was happening. Interpretation is the analyst's."),
        body_);
    caution->setWordWrap(true);
    caution->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; border: 1px solid %2; "
                                          "padding: 8px;")
                               .arg(colors::kTextSecondary.name(), colors::kBorder.name()));
    bodyLayout->addWidget(caution);

    layout->addWidget(body_);
    layout->addStretch();
    scroll->setWidget(content);

    clear();
}

void DetectionInspector::clear() {
    detection_.reset();
    run_.reset();
    evidence_.reset();
    headline_->setText(QStringLiteral("No detection selected"));
    placeholder_->setVisible(true);
    body_->setVisible(false);
}

void DetectionInspector::showDetection(const std::optional<Detection>& detection,
                                       const std::optional<AnalysisRun>& run,
                                       const std::optional<Evidence>& evidence) {
    detection_ = detection;
    run_ = run;
    evidence_ = evidence;
    if (!detection_) {
        clear();
        return;
    }
    placeholder_->setVisible(false);
    body_->setVisible(true);
    populate();
}

void DetectionInspector::populate() {
    const Detection& detection = *detection_;

    headline_->setText(QStringLiteral("%1 · %2 confidence")
                           .arg(QString::fromStdString(detection.classLabel))
                           .arg(detection.confidence * 100.0, 0, 'f', 1) +
                       QStringLiteral("%"));
    headline_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: 600;")
            .arg(detectionGroupColour(detection.classGroup).name()));

    setRowValue(observationForm_, kClassRow, QString::fromStdString(detection.classLabel));
    setRowValue(observationForm_, kGroupRow,
                QString::fromUtf8(toDisplayString(detection.classGroup)));
    setRowValue(observationForm_, kConfidenceRow,
                QStringLiteral("%1%").arg(detection.confidence * 100.0, 0, 'f', 2));
    setRowValue(observationForm_, kTimestampRow,
                QStringLiteral("%1  (%2 µs)")
                    .arg(timecode(detection.timestampUs))
                    .arg(detection.timestampUs));
    setRowValue(observationForm_, kFrameRow,
                detection.frameNumber ? QString::number(*detection.frameNumber)
                                      : QStringLiteral("Not available — variable frame timing"));

    setRowValue(geometryForm_, kNormalisedRow, boxText(detection.box));
    if (detection.pixelX && detection.pixelY && detection.pixelWidth && detection.pixelHeight) {
        setRowValue(geometryForm_, kPixelRow,
                    QStringLiteral("x %1  y %2  %3 × %4")
                        .arg(*detection.pixelX)
                        .arg(*detection.pixelY)
                        .arg(*detection.pixelWidth)
                        .arg(*detection.pixelHeight));
    } else {
        setRowValue(geometryForm_, kPixelRow, QString());
    }
    if (run_ && run_->sourceWidth && run_->sourceHeight) {
        setRowValue(geometryForm_, kSourceSizeRow,
                    QStringLiteral("%1 × %2").arg(*run_->sourceWidth).arg(*run_->sourceHeight));
    } else {
        setRowValue(geometryForm_, kSourceSizeRow, QString());
    }

    setRowValue(reviewForm_, kStateRow,
                QString::fromUtf8(toDisplayString(detection.verification)));
    // Beside the state, not buried: "confirmed" reached by examining this box
    // and "confirmed" reached by sweeping a filter are different claims, and an
    // analyst reading this row is often deciding whether to rely on one.
    setRowValue(reviewForm_, kReviewMethodRow,
                detection.verification == DetectionVerification::Unreviewed
                    ? QString()
                    : QString::fromUtf8(toDisplayString(detection.reviewMethod)));
    setRowValue(reviewForm_, kReviewedByRow, optionalText(detection.verifiedBy));
    setRowValue(reviewForm_, kReviewedAtRow, optionalTimestamp(detection.verifiedAt));
    setRowValue(reviewForm_, kNoteRow, QString::fromStdString(detection.analystNote));

    if (evidence_) {
        setRowValue(provenanceForm_, kEvidenceRow,
                    QStringLiteral("%1 · %2")
                        .arg(QString::fromStdString(evidence_->evidenceNumber),
                             QString::fromStdString(evidence_->originalFilename)));
    } else {
        setRowValue(provenanceForm_, kEvidenceRow, QString());
    }

    if (run_) {
        setRowValue(provenanceForm_, kEvidenceDigestRow,
                    formatDigestForDisplay(run_->evidenceSha256));
        setRowValue(provenanceForm_, kProviderRow,
                    QStringLiteral("%1 %2")
                        .arg(QString::fromStdString(run_->providerName),
                             QString::fromStdString(run_->providerVersion)));
        setRowValue(provenanceForm_, kRuntimeRow, QString::fromStdString(run_->runtime));
        setRowValue(provenanceForm_, kModelRow,
                    QStringLiteral("%1 %2")
                        .arg(QString::fromStdString(run_->modelName),
                             QString::fromStdString(run_->modelVersion)));
        setRowValue(provenanceForm_, kModelDigestRow,
                    run_->modelSha256 ? formatDigestForDisplay(*run_->modelSha256)
                                      : QStringLiteral("Not applicable — this provider runs no "
                                                       "model artefact"));
        setRowValue(provenanceForm_, kDeviceRow,
                    QStringLiteral("%1 (requested %2)")
                        .arg(QString::fromStdString(run_->deviceUsed.empty() ? "—"
                                                                             : run_->deviceUsed),
                             QString::fromStdString(run_->deviceRequested)));
        if (run_->samplingIntervalUs && *run_->samplingIntervalUs > 0) {
            setRowValue(provenanceForm_, kSamplingRow,
                        QStringLiteral("at most one analysed frame every %1 ms")
                            .arg(*run_->samplingIntervalUs / 1000));
        } else {
            setRowValue(provenanceForm_, kSamplingRow, QStringLiteral("every decoded frame"));
        }
        setRowValue(provenanceForm_, kRunRow, QString::fromStdString(run_->id));
        setRowValue(provenanceForm_, kRunStatusRow,
                    QString::fromUtf8(toDisplayString(run_->status)));
        setRowValue(provenanceForm_, kRunStartedRow,
                    timestamp(run_->startedAt.value_or(run_->createdAt)));
    }
    setRowValue(provenanceForm_, kDetectionIdRow, QString::fromStdString(detection.id));
}

}  // namespace trace::ui
