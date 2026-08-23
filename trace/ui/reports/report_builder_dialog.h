#pragma once

#include <QDialog>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/models/detection.h"
#include "core/models/evidence.h"
#include "reporting/report_service.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace trace::ui {

class ApplicationContext;
class BackgroundTask;

/// Builds an exhibit bundle from what the operator selects.
///
/// The export runs on a worker thread through the real ReportService: this dialog
/// chooses what goes in and reports what happened, and never claims an outcome it did
/// not receive from the service. A bundle is only reported as exported once the service
/// has written it and re-verified it.
class ReportBuilderDialog : public QDialog {
    Q_OBJECT

public:
    ReportBuilderDialog(ApplicationContext* context, QWidget* parent = nullptr);
    ~ReportBuilderDialog() override;

    /// Where the finished bundle was written, once one has been.
    const std::optional<ExportOutcome>& outcome() const { return outcome_; }

    // Used by the acceptance test to drive the real controls.
    QLineEdit* titleField() const { return titleField_; }
    QCheckBox* unconfirmedBox() const { return unconfirmedBox_; }
    QTreeWidget* evidenceTree() const { return evidenceTree_; }
    QTreeWidget* detectionTree() const { return detectionTree_; }
    QPushButton* exportButton() const { return exportButton_; }
    QPushButton* cancelButton() const { return cancelButton_; }
    /// Bypasses the directory chooser; the acceptance test supplies a path directly.
    void setDestinationOverride(const QString& path) { destinationOverride_ = path; }
    bool exportRunning() const { return exportInFlight_; }
    void startExport();

signals:
    void statusMessage(const QString& message, int timeoutMs);
    void exportFinished(bool succeeded, const QString& message);

private:
    void buildUi();
    void reloadEvidence();
    void reloadDetections();
    void updateSummary();
    void updateControlState();
    void applyProgressToUi();
    ReportDraft buildDraft() const;

    ApplicationContext* context_ = nullptr;
    std::vector<Evidence> evidence_;
    std::vector<Detection> detections_;

    std::unique_ptr<BackgroundTask> task_;
    std::shared_ptr<std::atomic<bool>> cancellation_;
    QTimer* progressTimer_ = nullptr;
    bool exportInFlight_ = false;
    QString destinationOverride_;

    /// Written by the export thread, read on the GUI thread by a timer.
    mutable std::mutex progressMutex_;
    ExportProgress latestProgress_;
    std::optional<ExportOutcome> outcome_;

    QLineEdit* titleField_ = nullptr;
    QCheckBox* unconfirmedBox_ = nullptr;
    QTreeWidget* evidenceTree_ = nullptr;
    QTreeWidget* detectionTree_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* noticeLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* progressLabel_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
};

}  // namespace trace::ui
