#include "ui/audio/audio_output.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/common/json.h"
#include "core/common/logging.h"
#include "media/ffmpeg/audio_decoder.h"

namespace trace::ui {
namespace {

constexpr const char* kComponent = "audio";

/// How much audio the device is asked to hold. Long enough that a busy machine
/// does not produce an audible gap, short enough that a pause or a seek takes
/// effect without the operator hearing the passage they have just left.
constexpr int kBufferMilliseconds = 200;

/// How often the buffer is topped up. Comfortably inside the buffer above, so a
/// tick that arrives late still finds audio left to play.
constexpr int kFeedIntervalMs = 20;

/// Chooses a format the device will accept and the decoder can produce.
///
/// Signed 16-bit interleaved is the one form every backend takes, and it is what
/// the decoder already resamples to, so the only open questions are rate and
/// channel count. The device's own preference is tried first because that is the
/// path with no conversion in the backend.
std::optional<QAudioFormat> negotiateFormat(const QAudioDevice& device) {
    QAudioFormat preferred = device.preferredFormat();
    preferred.setSampleFormat(QAudioFormat::Int16);
    if (device.isFormatSupported(preferred)) return preferred;

    for (int rate : {48000, 44100}) {
        for (int channels : {2, 1}) {
            QAudioFormat candidate;
            candidate.setSampleRate(rate);
            candidate.setChannelCount(channels);
            candidate.setSampleFormat(QAudioFormat::Int16);
            if (device.isFormatSupported(candidate)) return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// Engine — lives on the audio thread and is the only thing that touches the sink
// ---------------------------------------------------------------------------

class AudioOutput::Engine : public QObject {
    Q_OBJECT

public:
    explicit Engine(std::shared_ptr<AudioClock> clock) : clock_(std::move(clock)) {}

public slots:
    void openFile(const QString& path) {
        teardown();
        decoder_.reset();
        sourcePath_ = path;
    }

    void closeFile() {
        teardown();
        decoder_.reset();
        sourcePath_.clear();
    }

    void beginAt(qint64 fromUs) {
        teardown();
        if (sourcePath_.isEmpty() || silenced_) return;

        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull()) return;
        const auto format = negotiateFormat(device);
        if (!format) return;

        auto opened = AudioDecoder::open(sourcePath_.toStdString(), format->sampleRate(),
                                         format->channelCount());
        if (!opened) return;
        decoder_ = opened.take();

        if (fromUs > 0) {
            // A seek that cannot land is not a reason to play the wrong passage:
            // better to leave the track silent and let video pace itself.
            auto sought = decoder_->seek(fromUs);
            if (!sought.ok()) {
                decoder_.reset();
                return;
            }
        }

        sink_ = new QAudioSink(device, *format, this);
        sink_->setBufferSize(format->bytesForDuration(kBufferMilliseconds * 1000LL));
        sink_->setVolume(muted_ ? 0.0 : volume_);
        device_ = sink_->start();
        if (device_ == nullptr) {
            delete sink_;
            sink_ = nullptr;
            decoder_.reset();
            return;
        }

        exhausted_ = false;
        clock_->start(fromUs);

        timer_ = new QTimer(this);
        timer_->setInterval(kFeedIntervalMs);
        connect(timer_, &QTimer::timeout, this, &Engine::feed);
        timer_->start();
        feed();
    }

    void end() { teardown(); }

    void applyVolume(int percent) {
        volume_ = std::clamp(percent, 0, 100) / 100.0;
        if (sink_ != nullptr) sink_->setVolume(muted_ ? 0.0 : volume_);
    }

    void applyMuted(bool muted) {
        muted_ = muted;
        if (sink_ != nullptr) sink_->setVolume(muted_ ? 0.0 : volume_);
    }

    void applySpeed(double speed) {
        const bool silenced = !audioPlaysAtSpeed(speed);
        if (silenced == silenced_) return;
        silenced_ = silenced;
        // Changing speed while playing stops the track. It is not resumed here:
        // the controller restarts audio on the next play or seek, which is also
        // where the new position comes from.
        if (silenced_) teardown();
    }

private:
    void feed() {
        if (sink_ == nullptr || device_ == nullptr || decoder_ == nullptr) return;

        int free = sink_->bytesFree();
        while (free > 0) {
            if (pending_.empty()) {
                if (exhausted_) break;
                auto next = decoder_->nextBlock();
                if (!next) {
                    // The audio track can end before the video does. Stopping the
                    // clock hands pacing back to the steady clock so the rest of
                    // the item still plays at the right speed.
                    exhausted_ = true;
                    clock_->stop();
                    break;
                }
                const AudioBlockData block = next.take();
                const auto* bytes = reinterpret_cast<const char*>(block.samples.data());
                pending_.assign(bytes, bytes + block.samples.size() * sizeof(std::int16_t));
                pendingOffset_ = 0;
            }

            const auto available = static_cast<int>(pending_.size() - pendingOffset_);
            const int chunk = std::min(free, available);
            const qint64 written = device_->write(pending_.data() + pendingOffset_, chunk);
            if (written <= 0) break;
            pendingOffset_ += static_cast<std::size_t>(written);
            if (pendingOffset_ >= pending_.size()) {
                pending_.clear();
                pendingOffset_ = 0;
            }
            free -= static_cast<int>(written);
        }

        // The device's own count of what it has rendered. This, not the system
        // clock and not how much has been written, is what video is paced against.
        if (!exhausted_) clock_->reportDevicePosition(sink_->processedUSecs());
    }

    void teardown() {
        clock_->stop();
        if (timer_ != nullptr) {
            timer_->stop();
            delete timer_;
            timer_ = nullptr;
        }
        if (sink_ != nullptr) {
            sink_->stop();
            delete sink_;
            sink_ = nullptr;
        }
        device_ = nullptr;
        pending_.clear();
        pendingOffset_ = 0;
        exhausted_ = false;
        decoder_.reset();
    }

    std::shared_ptr<AudioClock> clock_;
    QString sourcePath_;
    std::unique_ptr<AudioDecoder> decoder_;
    QAudioSink* sink_ = nullptr;
    QIODevice* device_ = nullptr;  ///< owned by the sink
    QTimer* timer_ = nullptr;
    std::vector<char> pending_;
    std::size_t pendingOffset_ = 0;
    bool exhausted_ = false;
    bool silenced_ = false;
    bool muted_ = false;
    double volume_ = 0.8;
};

// ---------------------------------------------------------------------------
// AudioOutput — GUI-facing shell
// ---------------------------------------------------------------------------

AudioOutput::AudioOutput(QObject* parent)
    : QObject(parent), clock_(std::make_shared<AudioClock>()) {
    engine_ = new Engine(clock_);
    engine_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, engine_, &QObject::deleteLater);
    thread_.start();
}

AudioOutput::~AudioOutput() {
    // Stop the device before the thread goes away, so the sink is destroyed on
    // the thread that created it.
    if (engine_ != nullptr) {
        QMetaObject::invokeMethod(engine_, "end", Qt::BlockingQueuedConnection);
    }
    thread_.quit();
    thread_.wait();
}

bool AudioOutput::deviceAvailable() {
    return !QMediaDevices::defaultAudioOutput().isNull();
}

bool AudioOutput::open(const QString& absolutePath) {
    hasAudio_ = false;
    unavailableReason_.clear();

    if (!deviceAvailable()) {
        unavailableReason_ = QStringLiteral("This machine has no audio output device.");
        return false;
    }

    // Probing here rather than on the audio thread means the viewer can label its
    // controls correctly straight away, instead of enabling them and then finding
    // out there was nothing to play.
    auto probe = AudioDecoder::open(absolutePath.toStdString(), 48000, 2);
    if (!probe) {
        unavailableReason_ = probe.error().code() == ErrorCode::NotFound
                                 ? QStringLiteral("This item has no audio track.")
                                 : QStringLiteral("The audio track could not be decoded.");
        logInfo(kComponent, "Audio unavailable for playback",
                JsonValue::object().set("detail", probe.error().toString()));
        return false;
    }

    hasAudio_ = true;
    QMetaObject::invokeMethod(engine_, "openFile", Qt::QueuedConnection,
                              Q_ARG(QString, absolutePath));
    return true;
}

void AudioOutput::close() {
    hasAudio_ = false;
    unavailableReason_.clear();
    QMetaObject::invokeMethod(engine_, "closeFile", Qt::QueuedConnection);
}

void AudioOutput::start(Microseconds fromUs) {
    if (!hasAudio_) return;
    QMetaObject::invokeMethod(engine_, "beginAt", Qt::QueuedConnection,
                              Q_ARG(qint64, static_cast<qint64>(fromUs)));
}

void AudioOutput::stop() {
    QMetaObject::invokeMethod(engine_, "end", Qt::QueuedConnection);
}

void AudioOutput::setVolume(int percent) {
    QMetaObject::invokeMethod(engine_, "applyVolume", Qt::QueuedConnection, Q_ARG(int, percent));
}

void AudioOutput::setMuted(bool muted) {
    QMetaObject::invokeMethod(engine_, "applyMuted", Qt::QueuedConnection, Q_ARG(bool, muted));
}

void AudioOutput::setSpeed(double speed) {
    QMetaObject::invokeMethod(engine_, "applySpeed", Qt::QueuedConnection, Q_ARG(double, speed));
}

std::function<std::optional<Microseconds>()> AudioOutput::clockSource() const {
    // A weak reference, because the playback engine's worker thread may still be
    // between frames when the viewer closes. Holding the clock alive for the
    // duration of one call is cheaper than making the engine wait for it.
    std::weak_ptr<AudioClock> weak = clock_;
    return [weak]() -> std::optional<Microseconds> {
        if (auto clock = weak.lock()) return clock->positionUs();
        return std::nullopt;
    };
}

}  // namespace trace::ui

#include "audio_output.moc"
