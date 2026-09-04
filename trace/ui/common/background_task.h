#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <thread>

namespace trace::ui {

/// Runs one long operation off the GUI thread.
///
/// File copying, hashing, metadata extraction and integrity verification all go
/// through this: the work runs on a worker thread and reports back with queued
/// signals, so the window keeps repainting and the operator can cancel.
class BackgroundTask : public QObject {
    Q_OBJECT

public:
    using Work = std::function<void(BackgroundTask&)>;

    explicit BackgroundTask(QObject* parent = nullptr);
    ~BackgroundTask() override;

    void start(Work work);
    void requestCancellation();
    bool cancellationRequested() const { return cancelled_.load(); }
    bool running() const { return running_.load(); }

    /// Callable from the worker thread; delivery is marshalled by Qt.
    void reportProgress(int percent, const QString& stage, const QString& message);
    void reportFinished(bool succeeded, const QString& message);

    /// Blocks until the worker has finished. Used on shutdown.
    void wait();

signals:
    void progressChanged(int percent, QString stage, QString message);
    void finished(bool succeeded, QString message);

private:
    std::thread thread_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};
};

}  // namespace trace::ui
