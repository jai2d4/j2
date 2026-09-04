#include "ui/common/background_task.h"

#include <utility>

namespace trace::ui {

BackgroundTask::BackgroundTask(QObject* parent) : QObject(parent) {}

BackgroundTask::~BackgroundTask() {
    requestCancellation();
    wait();
}

void BackgroundTask::start(Work work) {
    if (running_.load()) return;
    running_.store(true);
    cancelled_.store(false);
    thread_ = std::thread([this, work = std::move(work)]() mutable {
        work(*this);
        running_.store(false);
    });
}

void BackgroundTask::requestCancellation() { cancelled_.store(true); }

void BackgroundTask::reportProgress(int percent, const QString& stage, const QString& message) {
    emit progressChanged(percent, stage, message);
}

void BackgroundTask::reportFinished(bool succeeded, const QString& message) {
    emit finished(succeeded, message);
}

void BackgroundTask::wait() {
    if (thread_.joinable()) thread_.join();
}

}  // namespace trace::ui
