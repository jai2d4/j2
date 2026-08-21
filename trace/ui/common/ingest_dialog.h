#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <memory>
#include <optional>

#include "core/services/evidence_service.h"

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace trace::ui {

class ApplicationContext;
class BackgroundTask;

/// Shows the real stages of an import — copy, verify, metadata, record — while
/// the work happens on a worker thread. Cancelling rolls the import back; the
/// dialog never reports success for a partial import.
class IngestDialog : public QDialog {
    Q_OBJECT

public:
    IngestDialog(ApplicationContext* context, QStringList sourcePaths, QWidget* parent = nullptr);
    ~IngestDialog() override;

    int importedCount() const { return imported_; }
    /// True once every requested file has been processed (imported or failed).
    bool finished() const { return finished_; }
    QStringList failures() const { return failures_; }
    const std::optional<Evidence>& lastImported() const { return lastImported_; }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void startNext();
    void markStage(IngestStage stage);
    void appendFailure(const QString& file, const QString& message);

    ApplicationContext* context_ = nullptr;
    QStringList sources_;
    int index_ = 0;
    int imported_ = 0;
    std::optional<Evidence> lastImported_;
    QStringList failures_;

    QLabel* fileLabel_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QListWidget* stageList_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    std::unique_ptr<BackgroundTask> task_;
    bool finished_ = false;
};

}  // namespace trace::ui
