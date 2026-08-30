#pragma once

#include <QWidget>

#include <optional>
#include <vector>

#include "core/models/analysis_run.h"
#include "core/models/detection.h"
#include "core/models/evidence.h"
#include "core/repositories/analysis_repository.h"
#include "ui/common/detection_overlay.h"
#include "ui/timeline/timeline_widget.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;

namespace trace::ui {

class ApplicationContext;

/// Browses what a detection run found, and records the analyst's review of it.
///
/// The table is the authoritative list; the video overlay and the timeline lanes
/// are two other views of the same filtered query, so what is drawn on the frame
/// and what is listed here can never disagree.
///
/// Review decisions are stored and audited. Rejecting a detection does not
/// delete it: it stays in the record, marked as rejected, because the fact that
/// the model reported it is part of the analytical history.
class DetectionsPanel : public QWidget {
    Q_OBJECT

public:
    explicit DetectionsPanel(ApplicationContext* context, QWidget* parent = nullptr);

    void setEvidence(const std::optional<Evidence>& evidence);
    /// Results are always read from one identified run, so every row on screen
    /// can name the model that produced it.
    void setResultsRun(const std::optional<AnalysisRun>& run);
    void reload();

    /// The filter as the operator has it set.
    DetectionQuery currentQuery() const;
    /// Boxes for the analysed frame nearest `positionUs`, honouring the filter.
    DetectionOverlayFrame overlayAt(qint64 positionUs) const;
    /// One presence lane per class group, built from the same filter.
    std::vector<TimelineTrack> timelineTracks() const;

    void selectDetection(const QString& detectionId);
    QString selectedDetectionId() const;
    std::optional<Detection> selectedDetection() const;
    /// Selects the first detection at or after `positionUs`, used when the
    /// operator activates a timeline lane marker.
    void selectNearestTo(qint64 positionUs);

    void confirmSelected();
    void rejectSelected();
    void markSelectedUncertain();
    void resetSelectedReview();

    /// Moves the selection. `onlyUnreviewed` skips anything already ruled on,
    /// which is what makes a second pass through a part-reviewed run practical.
    void selectNext(bool onlyUnreviewed = false);
    void selectPrevious();

    /// Applies one decision to everything the filter currently matches, after
    /// asking. Returns how many rows changed, or nothing if the operator
    /// declined — a sweep is not something to do on a mis-keyed shortcut.
    std::optional<std::int64_t> reviewAllMatching(DetectionVerification state,
                                                  bool skipConfirmation = false);

    /// Whether a decision moves to the next unreviewed detection by itself.
    /// On by default: without it, reviewing is click, decide, click, decide,
    /// and the clicking is most of the work.
    void setAutoAdvance(bool on);
    bool autoAdvance() const { return autoAdvance_; }

    /// How much of the current run has been ruled on.
    ReviewProgress progress() const { return progress_; }

    const std::vector<Detection>& loadedDetections() const { return detections_; }
    std::int64_t totalMatching() const { return totalMatching_; }

    // Used by the acceptance tests to drive the real controls.
    QTreeWidget* tree() const { return tree_; }
    QComboBox* groupFilter() const { return groupFilter_; }
    QComboBox* classFilter() const { return classFilter_; }
    QComboBox* reviewFilter() const { return reviewFilter_; }
    QDoubleSpinBox* confidenceFilter() const { return confidenceFilter_; }
    QCheckBox* includeRejectedBox() const { return includeRejectedBox_; }

signals:
    void statusMessage(const QString& message, int timeoutMs);
    /// Click-to-jump: the operator activated a row.
    void jumpRequested(qint64 positionUs);
    void detectionSelected(const QString& detectionId);
    void filtersChanged();
    void verificationChanged();

private:
    void buildUi();
    void reloadClassFilter();
    void applyVerification(DetectionVerification state);
    void updateActionState();
    void refreshProgress();
    /// The filter in words, for the confirmation dialog and the audit record —
    /// one description, so what the operator agreed to and what is recorded
    /// cannot drift apart.
    QString filterDescription() const;
    /// How far from the playhead an analysed frame may be and still be drawn.
    /// Taken from the run's recorded sampling interval, never guessed from the
    /// spacing of the detections themselves.
    qint64 overlayToleranceUs() const;

    ApplicationContext* context_ = nullptr;
    std::optional<Evidence> evidence_;
    std::optional<AnalysisRun> run_;
    std::vector<Detection> detections_;
    std::int64_t totalMatching_ = 0;

    QComboBox* groupFilter_ = nullptr;
    QComboBox* classFilter_ = nullptr;
    QComboBox* reviewFilter_ = nullptr;
    QDoubleSpinBox* confidenceFilter_ = nullptr;
    QCheckBox* includeRejectedBox_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* runLabel_ = nullptr;
    QLineEdit* noteEdit_ = nullptr;
    QPushButton* confirmButton_ = nullptr;
    QPushButton* rejectButton_ = nullptr;
    QPushButton* uncertainButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* jumpButton_ = nullptr;
    QPushButton* bulkButton_ = nullptr;
    QComboBox* bulkStateBox_ = nullptr;
    QCheckBox* autoAdvanceBox_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* progressLabel_ = nullptr;

    ReviewProgress progress_;
    bool autoAdvance_ = true;
};

}  // namespace trace::ui
