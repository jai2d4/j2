#pragma once

#include <QWidget>

#include <optional>

#include "core/models/analysis_run.h"
#include "core/models/detection.h"
#include "core/models/evidence.h"

class QFormLayout;
class QLabel;

namespace trace::ui {

/// Everything known about one detection, and where it came from.
///
/// This is the provenance view: what was reported, at which moment of which
/// evidence item, by which model artefact, on which device, and what a human
/// analyst has since said about it. Nothing here is inferred — a field the
/// record does not carry reads "Not available".
///
/// TRACE states a visual class and a confidence. It does not identify people,
/// read plates, or draw conclusions, and this panel says so where an operator
/// will see it.
class DetectionInspector : public QWidget {
    Q_OBJECT

public:
    explicit DetectionInspector(QWidget* parent = nullptr);

    void showDetection(const std::optional<Detection>& detection,
                       const std::optional<AnalysisRun>& run,
                       const std::optional<Evidence>& evidence);
    void clear();

    const std::optional<Detection>& detection() const { return detection_; }

private:
    void buildUi();
    void populate();

    std::optional<Detection> detection_;
    std::optional<AnalysisRun> run_;
    std::optional<Evidence> evidence_;

    QLabel* headline_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QWidget* body_ = nullptr;
    QFormLayout* observationForm_ = nullptr;
    QFormLayout* geometryForm_ = nullptr;
    QFormLayout* reviewForm_ = nullptr;
    QFormLayout* provenanceForm_ = nullptr;
};

}  // namespace trace::ui
