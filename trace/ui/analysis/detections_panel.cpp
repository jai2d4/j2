#include "ui/analysis/detections_panel.h"

#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>

#include "ui/app/application_context.h"
#include "ui/common/display_utils.h"
#include "ui/common/theme.h"

namespace trace::ui {
namespace {

/// The table can hold a very long recording's worth of rows. Loading every one
/// of them would stall the window for no benefit — the operator filters instead.
/// The count of everything matching the filter is still shown, so the limit is
/// visible rather than silent.
constexpr int kMaxRows = 5000;
/// Analysed moments closer together than this are drawn as one lane span.
constexpr qint64 kLaneJoinGapUs = 600'000;
constexpr qint64 kLaneMinimumSpanUs = 120'000;

enum Column { kTime = 0, kClass, kGroup, kConfidence, kReview, kNote };

/// Sorts by the real value rather than by the displayed text, so 00:00:09.5
/// comes before 00:00:10.0 and 9% before 10%.
class DetectionRow : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override {
        const int column = treeWidget() != nullptr ? treeWidget()->sortColumn() : 0;
        const QVariant mine = data(column, Qt::UserRole + 1);
        const QVariant theirs = other.data(column, Qt::UserRole + 1);
        if (mine.isValid() && theirs.isValid()) return mine.toDouble() < theirs.toDouble();
        return QTreeWidgetItem::operator<(other);
    }
};

QString reviewText(DetectionVerification state) {
    return QString::fromUtf8(toDisplayString(state));
}

QColor reviewColour(DetectionVerification state) {
    switch (state) {
        case DetectionVerification::Confirmed:
            return colors::kVerified;
        case DetectionVerification::Rejected:
            return colors::kFailure;
        case DetectionVerification::Uncertain:
            return colors::kWarning;
        case DetectionVerification::Unreviewed:
            break;
    }
    return colors::kTextSecondary;
}

}  // namespace

DetectionsPanel::DetectionsPanel(ApplicationContext* context, QWidget* parent)
    : QWidget(parent), context_(context) {
    buildUi();
    updateActionState();
}

void DetectionsPanel::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    runLabel_ = new QLabel(QStringLiteral("No analysis results"), this);
    runLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    runLabel_->setWordWrap(true);
    layout->addWidget(runLabel_);

    // ------------------------------------------------------------- filtering
    auto* filters = new QGridLayout();
    filters->setHorizontalSpacing(8);
    filters->setVerticalSpacing(4);

    const auto filterLabel = [this](const QString& text) {
        auto* label = new QLabel(text, this);
        label->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
        return label;
    };

    groupFilter_ = new QComboBox(this);
    groupFilter_->addItem(QStringLiteral("All groups"), QString());
    for (const auto group : {DetectionClassGroup::Person, DetectionClassGroup::Vehicle,
                             DetectionClassGroup::Object}) {
        groupFilter_->addItem(QString::fromUtf8(toDisplayString(group)),
                              QString::fromUtf8(toString(group)));
    }
    filters->addWidget(filterLabel(QStringLiteral("Group")), 0, 0);
    filters->addWidget(groupFilter_, 0, 1);

    classFilter_ = new QComboBox(this);
    classFilter_->addItem(QStringLiteral("All classes"), QString());
    filters->addWidget(filterLabel(QStringLiteral("Class")), 0, 2);
    filters->addWidget(classFilter_, 0, 3);

    reviewFilter_ = new QComboBox(this);
    reviewFilter_->addItem(QStringLiteral("Any review state"), QString());
    for (const auto state : {DetectionVerification::Unreviewed, DetectionVerification::Confirmed,
                             DetectionVerification::Rejected, DetectionVerification::Uncertain}) {
        reviewFilter_->addItem(reviewText(state), QString::fromUtf8(toString(state)));
    }
    filters->addWidget(filterLabel(QStringLiteral("Review")), 1, 0);
    filters->addWidget(reviewFilter_, 1, 1);

    confidenceFilter_ = new QDoubleSpinBox(this);
    confidenceFilter_->setRange(0.0, 0.99);
    confidenceFilter_->setSingleStep(0.05);
    confidenceFilter_->setDecimals(2);
    confidenceFilter_->setValue(0.0);
    confidenceFilter_->setToolTip(
        QStringLiteral("Hide detections the model scored below this. Nothing is deleted."));
    filters->addWidget(filterLabel(QStringLiteral("Min. confidence")), 1, 2);
    filters->addWidget(confidenceFilter_, 1, 3);

    includeRejectedBox_ = new QCheckBox(QStringLiteral("Show rejected"), this);
    includeRejectedBox_->setChecked(false);
    includeRejectedBox_->setToolTip(
        QStringLiteral("Rejected detections are kept permanently. This only controls whether they "
                       "are listed and drawn."));
    filters->addWidget(includeRejectedBox_, 2, 0, 1, 2);
    layout->addLayout(filters);

    // ----------------------------------------------------------------- table
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(6);
    tree_->setHeaderLabels({QStringLiteral("Time"), QStringLiteral("Class"),
                            QStringLiteral("Group"), QStringLiteral("Confidence"),
                            QStringLiteral("Review"), QStringLiteral("Note")});
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(kTime, Qt::AscendingOrder);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->header()->setSectionResizeMode(kTime, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(kNote, QHeaderView::Stretch);
    layout->addWidget(tree_, 1);

    summaryLabel_ = new QLabel(QString(), this);
    summaryLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    summaryLabel_->setWordWrap(true);
    layout->addWidget(summaryLabel_);

    // ---------------------------------------------------------------- review
    noteEdit_ = new QLineEdit(this);
    noteEdit_->setPlaceholderText(QStringLiteral("Reviewer note (optional) — saved with the "
                                                 "decision and written to the audit trail"));
    layout->addWidget(noteEdit_);

    auto* actions = new QHBoxLayout();
    confirmButton_ = new QPushButton(QStringLiteral("Confirm"), this);
    rejectButton_ = new QPushButton(QStringLiteral("Reject"), this);
    uncertainButton_ = new QPushButton(QStringLiteral("Uncertain"), this);
    resetButton_ = new QPushButton(QStringLiteral("Clear review"), this);
    jumpButton_ = new QPushButton(QStringLiteral("Jump to frame"), this);
    styleAsPrimaryAction(jumpButton_);
    confirmButton_->setToolTip(
        QStringLiteral("Record that a human analyst agrees with this detection."));
    rejectButton_->setToolTip(
        QStringLiteral("Record that a human analyst disagrees. The detection is kept and marked "
                       "rejected — it is never deleted."));
    uncertainButton_->setToolTip(
        QStringLiteral("Record that the analyst could not decide from this footage."));
    resetButton_->setToolTip(QStringLiteral("Return this detection to unreviewed."));
    for (auto* button : {confirmButton_, rejectButton_, uncertainButton_, resetButton_}) {
        actions->addWidget(button);
    }
    actions->addStretch();
    actions->addWidget(jumpButton_);
    layout->addLayout(actions);

    const auto filterChanged = [this] {
        reload();
        emit filtersChanged();
    };
    connect(groupFilter_, &QComboBox::currentIndexChanged, this, filterChanged);
    connect(classFilter_, &QComboBox::currentIndexChanged, this, filterChanged);
    connect(reviewFilter_, &QComboBox::currentIndexChanged, this, filterChanged);
    connect(confidenceFilter_, &QDoubleSpinBox::valueChanged, this, filterChanged);
    connect(includeRejectedBox_, &QCheckBox::toggled, this, filterChanged);

    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        updateActionState();
        emit detectionSelected(selectedDetectionId());
    });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr) return;
        emit jumpRequested(item->data(kTime, Qt::UserRole + 1).toLongLong());
    });
    connect(jumpButton_, &QPushButton::clicked, this, [this] {
        auto* item = tree_->currentItem();
        if (item == nullptr) return;
        emit jumpRequested(item->data(kTime, Qt::UserRole + 1).toLongLong());
    });
    connect(confirmButton_, &QPushButton::clicked, this, &DetectionsPanel::confirmSelected);
    connect(rejectButton_, &QPushButton::clicked, this, &DetectionsPanel::rejectSelected);
    connect(uncertainButton_, &QPushButton::clicked, this, &DetectionsPanel::markSelectedUncertain);
    connect(resetButton_, &QPushButton::clicked, this, &DetectionsPanel::resetSelectedReview);
}

void DetectionsPanel::setEvidence(const std::optional<Evidence>& evidence) {
    evidence_ = evidence;
    run_.reset();
    reloadClassFilter();
    reload();
}

void DetectionsPanel::setResultsRun(const std::optional<AnalysisRun>& run) {
    run_ = run;
    reloadClassFilter();
    reload();
}

void DetectionsPanel::reloadClassFilter() {
    const QString previous = classFilter_->currentData().toString();
    const QSignalBlocker blocker(classFilter_);
    classFilter_->clear();
    classFilter_->addItem(QStringLiteral("All classes"), QString());
    if (!evidence_ || !context_->isInitialised()) return;

    auto counts = context_->analysis().classCounts(evidence_->id);
    if (!counts) return;
    for (const auto& [label, count] : counts.value()) {
        classFilter_->addItem(
            QStringLiteral("%1 (%2)").arg(QString::fromStdString(label)).arg(count),
            QString::fromStdString(label));
    }
    const int index = classFilter_->findData(previous);
    if (index >= 0) classFilter_->setCurrentIndex(index);
}

DetectionQuery DetectionsPanel::currentQuery() const {
    DetectionQuery query;
    if (evidence_) query.evidenceId = evidence_->id;
    if (run_) query.analysisRunId = run_->id;

    const QString group = groupFilter_->currentData().toString();
    if (!group.isEmpty()) query.classGroup = detectionClassGroupFromString(group.toStdString());
    const QString review = reviewFilter_->currentData().toString();
    if (!review.isEmpty()) {
        query.verification = detectionVerificationFromString(review.toStdString());
    }
    query.minimumConfidence = confidenceFilter_->value();
    query.includeRejected = includeRejectedBox_->isChecked();
    return query;
}

void DetectionsPanel::reload() {
    // A queued signal can arrive after the application has released its
    // services on shutdown; there is nothing to read at that point.
    if (!context_->isInitialised()) return;

    const QString previouslySelected = selectedDetectionId();
    detections_.clear();
    totalMatching_ = 0;
    tree_->clear();

    if (!evidence_) {
        runLabel_->setText(QStringLiteral("No evidence open"));
        summaryLabel_->setText(QString());
        updateActionState();
        return;
    }
    if (!run_) {
        runLabel_->setText(
            QStringLiteral("This item has no analysis results. Run an analysis from the AI "
                           "analysis panel."));
        summaryLabel_->setText(QString());
        updateActionState();
        return;
    }

    runLabel_->setText(
        QStringLiteral("Results from %1 %2 · %3 · run %4")
            .arg(QString::fromStdString(run_->modelName),
                 QString::fromStdString(run_->modelVersion),
                 QString::fromUtf8(toDisplayString(run_->status)),
                 QString::fromStdString(run_->id.substr(0, 8))));

    DetectionQuery query = currentQuery();
    auto counted = context_->analysis().countDetections(query);
    if (counted) totalMatching_ = counted.value();

    query.limit = kMaxRows;
    auto loaded = context_->analysis().detections(query);
    if (!loaded) {
        emit statusMessage(QStringLiteral("Detections could not be read: %1")
                               .arg(QString::fromStdString(loaded.error().message())),
                           8000);
        updateActionState();
        return;
    }
    detections_ = loaded.take();

    // The class filter is applied here rather than in SQL: the query filters by
    // group, and the label list is short enough that filtering it in memory
    // keeps the repository's filter surface small.
    const QString classLabel = classFilter_->currentData().toString();

    tree_->setSortingEnabled(false);
    std::int64_t shown = 0;
    for (const auto& detection : detections_) {
        if (!classLabel.isEmpty() &&
            QString::fromStdString(detection.classLabel) != classLabel) {
            continue;
        }
        auto* item = new DetectionRow(tree_);
        item->setText(kTime, timecode(detection.timestampUs));
        item->setData(kTime, Qt::UserRole + 1, static_cast<qlonglong>(detection.timestampUs));
        item->setData(kTime, Qt::UserRole, QString::fromStdString(detection.id));
        item->setFont(kTime, monospaceFont(-1));

        item->setText(kClass, QString::fromStdString(detection.classLabel));
        item->setForeground(kClass, QBrush(detectionGroupColour(detection.classGroup)));
        item->setText(kGroup, QString::fromUtf8(toDisplayString(detection.classGroup)));
        item->setText(kConfidence,
                      QStringLiteral("%1%").arg(detection.confidence * 100.0, 0, 'f', 1));
        item->setData(kConfidence, Qt::UserRole + 1, detection.confidence);
        item->setTextAlignment(kConfidence, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(kReview, reviewText(detection.verification));
        item->setForeground(kReview, QBrush(reviewColour(detection.verification)));
        item->setText(kNote, QString::fromStdString(detection.analystNote));
        item->setToolTip(kClass,
                         QStringLiteral("%1 at %2 — reported by %3 %4")
                             .arg(QString::fromStdString(detection.classLabel),
                                  timecode(detection.timestampUs),
                                  QString::fromStdString(run_->modelName),
                                  QString::fromStdString(run_->modelVersion)));
        ++shown;
    }
    tree_->setSortingEnabled(true);

    QString summary = QStringLiteral("%1 detection%2 listed")
                          .arg(shown)
                          .arg(shown == 1 ? QString() : QStringLiteral("s"));
    if (totalMatching_ > static_cast<std::int64_t>(detections_.size())) {
        summary += QStringLiteral(" — first %1 of %2 matching rows; narrow the filter to see the "
                                  "rest")
                       .arg(detections_.size())
                       .arg(totalMatching_);
    }
    if (!includeRejectedBox_->isChecked()) {
        summary += QStringLiteral(". Rejected detections are hidden, not deleted.");
    }
    summaryLabel_->setText(summary);

    if (!previouslySelected.isEmpty()) selectDetection(previouslySelected);
    updateActionState();
}

QString DetectionsPanel::selectedDetectionId() const {
    auto* item = tree_->currentItem();
    if (item == nullptr || !item->isSelected()) return {};
    return item->data(kTime, Qt::UserRole).toString();
}

std::optional<Detection> DetectionsPanel::selectedDetection() const {
    const QString id = selectedDetectionId();
    if (id.isEmpty()) return std::nullopt;
    for (const auto& detection : detections_) {
        if (QString::fromStdString(detection.id) == id) return detection;
    }
    return std::nullopt;
}

void DetectionsPanel::selectDetection(const QString& detectionId) {
    if (detectionId.isEmpty()) {
        tree_->clearSelection();
        tree_->setCurrentItem(nullptr);
        updateActionState();
        return;
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* item = tree_->topLevelItem(i);
        if (item->data(kTime, Qt::UserRole).toString() != detectionId) continue;
        tree_->setCurrentItem(item);
        tree_->scrollToItem(item);
        return;
    }
}

void DetectionsPanel::selectNearestTo(qint64 positionUs) {
    QTreeWidgetItem* best = nullptr;
    qint64 bestDelta = 0;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* item = tree_->topLevelItem(i);
        const qint64 timestamp = item->data(kTime, Qt::UserRole + 1).toLongLong();
        const qint64 delta = std::abs(timestamp - positionUs);
        if (best == nullptr || delta < bestDelta) {
            best = item;
            bestDelta = delta;
        }
    }
    if (best == nullptr) return;
    tree_->setCurrentItem(best);
    tree_->scrollToItem(best);
}

qint64 DetectionsPanel::overlayToleranceUs() const {
    if (!run_ || !run_->samplingIntervalUs || *run_->samplingIntervalUs <= 0) {
        // Every decoded frame was analysed (or the interval was not recorded):
        // only an analysed frame essentially on the playhead may be drawn.
        return 40'000;
    }
    return *run_->samplingIntervalUs;
}

DetectionOverlayFrame DetectionsPanel::overlayAt(qint64 positionUs) const {
    DetectionOverlayFrame frame;
    if (!evidence_ || !run_ || !context_->isInitialised()) return frame;

    DetectionQuery query = currentQuery();
    auto nearest = context_->analysis().detectionsNearTimestamp(
        evidence_->id, positionUs, overlayToleranceUs(), query.minimumConfidence,
        query.includeRejected);
    if (!nearest) return frame;

    const QString classLabel = classFilter_->currentData().toString();
    const QString selected = selectedDetectionId();
    for (const auto& detection : nearest.value()) {
        if (detection.analysisRunId != run_->id) continue;
        if (query.classGroup && detection.classGroup != *query.classGroup) continue;
        if (query.verification && detection.verification != *query.verification) continue;
        if (!classLabel.isEmpty() && QString::fromStdString(detection.classLabel) != classLabel) {
            continue;
        }
        DetectionOverlayItem item;
        item.id = QString::fromStdString(detection.id);
        item.box = detection.box;
        item.label = QString::fromStdString(detection.classLabel);
        item.confidence = detection.confidence;
        item.group = detection.classGroup;
        item.verification = detection.verification;
        item.selected = item.id == selected;
        frame.timestampUs = detection.timestampUs;
        frame.items.push_back(std::move(item));
    }
    return frame;
}

std::vector<TimelineTrack> DetectionsPanel::timelineTracks() const {
    std::vector<TimelineTrack> tracks;
    if (!evidence_ || !run_ || !context_->isInitialised()) return tracks;

    auto points = context_->analysis().detectionTimeline(currentQuery());
    if (!points) return tracks;

    const QString classLabel = classFilter_->currentData().toString();
    // A label filter cannot be answered by the grouped query, so when one is set
    // the lanes are built from the rows already loaded instead — the two paths
    // produce the same spans for the same filter.
    std::vector<DetectionTimelinePoint> source;
    if (classLabel.isEmpty()) {
        source = points.take();
    } else {
        for (const auto& detection : detections_) {
            if (QString::fromStdString(detection.classLabel) != classLabel) continue;
            if (!source.empty() && source.back().group == detection.classGroup &&
                source.back().timestampUs == detection.timestampUs) {
                ++source.back().count;
                continue;
            }
            source.push_back(
                DetectionTimelinePoint{detection.classGroup, detection.timestampUs, 1});
        }
    }

    const qint64 tolerance = overlayToleranceUs();
    const qint64 joinGap = std::max(kLaneJoinGapUs, tolerance * 2);

    for (const auto group : {DetectionClassGroup::Person, DetectionClassGroup::Vehicle,
                             DetectionClassGroup::Object}) {
        TimelineTrack track;
        track.name = QString::fromUtf8(toDisplayString(group));
        track.colour = detectionGroupColour(group);

        // Analysed moments are merged into presence spans: one marker per
        // sampled frame would be unreadable on an hour of footage, and the
        // question an analyst asks the timeline is "when is there a person",
        // not "which frames were sampled".
        std::vector<std::pair<qint64, qint64>> spans;
        std::vector<std::int64_t> counts;
        for (const auto& point : source) {
            if (point.group != group) continue;
            if (!spans.empty() && point.timestampUs - spans.back().second <= joinGap) {
                spans.back().second = point.timestampUs;
                counts.back() = std::max<std::int64_t>(counts.back(), point.count);
                continue;
            }
            spans.emplace_back(point.timestampUs, point.timestampUs);
            counts.push_back(point.count);
        }

        for (std::size_t i = 0; i < spans.size(); ++i) {
            TimelineMarker marker;
            marker.id = QStringLiteral("%1:%2")
                            .arg(QString::fromUtf8(toString(group)))
                            .arg(spans[i].first);
            marker.startUs = spans[i].first;
            marker.endUs = std::max(spans[i].second, spans[i].first + kLaneMinimumSpanUs);
            marker.label = QStringLiteral("%1 — up to %2 at once")
                               .arg(QString::fromUtf8(toDisplayString(group)))
                               .arg(counts[i]);
            marker.colour = track.colour;
            track.markers.push_back(std::move(marker));
        }
        if (!track.markers.empty()) tracks.push_back(std::move(track));
    }
    return tracks;
}

void DetectionsPanel::confirmSelected() { applyVerification(DetectionVerification::Confirmed); }
void DetectionsPanel::rejectSelected() { applyVerification(DetectionVerification::Rejected); }
void DetectionsPanel::markSelectedUncertain() {
    applyVerification(DetectionVerification::Uncertain);
}
void DetectionsPanel::resetSelectedReview() {
    applyVerification(DetectionVerification::Unreviewed);
}

void DetectionsPanel::applyVerification(DetectionVerification state) {
    if (!context_->isInitialised()) return;
    auto detection = selectedDetection();
    if (!detection) {
        emit statusMessage(QStringLiteral("Select a detection to review first."), 4000);
        return;
    }
    if (!evidence_) return;

    const QString note = noteEdit_->text().trimmed();
    auto status = context_->analysis().setVerification(
        *detection, state, note.toStdString(), context_->currentCaseNumber().toStdString(),
        evidence_->evidenceNumber);
    if (!status) {
        emit statusMessage(QStringLiteral("Review not recorded: %1")
                               .arg(QString::fromStdString(status.error().message())),
                           8000);
        return;
    }

    noteEdit_->clear();
    const QString selectedId = QString::fromStdString(detection->id);
    context_->notifyDetectionsChanged();
    context_->notifyAuditChanged();
    reload();
    selectDetection(selectedId);
    emit statusMessage(QStringLiteral("%1 at %2 marked %3")
                           .arg(QString::fromStdString(detection->classLabel),
                                timecode(detection->timestampUs),
                                reviewText(state).toLower()),
                       6000);
    emit verificationChanged();
}

void DetectionsPanel::updateActionState() {
    const bool hasSelection = selectedDetection().has_value();
    for (auto* button : {confirmButton_, rejectButton_, uncertainButton_, resetButton_,
                         jumpButton_}) {
        button->setEnabled(hasSelection);
    }
    noteEdit_->setEnabled(hasSelection);
}

}  // namespace trace::ui
