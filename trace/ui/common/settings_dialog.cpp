#include "ui/common/settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <sqlite3.h>

#include "ai/interfaces/provider_registry.h"
#include "core/common/logging.h"
#include "core/common/string_utils.h"
#include "core/settings/system_capabilities.h"
#include "media/ffmpeg/ffmpeg_support.h"
#include "media/playback/playback_controller.h"
#include "trace/trace_version.h"
#include "ui/app/application_context.h"
#include "ui/common/theme.h"
#include "ui/common/display_utils.h"

namespace trace::ui {
namespace {

QLabel* reservedNote(QWidget* parent, const QString& text) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextDisabled.name()));
    return label;
}

}  // namespace

SettingsDialog::SettingsDialog(ApplicationContext* context, QWidget* parent)
    : QDialog(parent), context_(context) {
    setWindowTitle(QStringLiteral("TRACE settings"));
    setModal(true);
    setMinimumWidth(620);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(14);

    // ---------------------------------------------------------------- storage
    auto* storageBox = new QGroupBox(QStringLiteral("STORAGE"), this);
    auto* storageForm = new QFormLayout(storageBox);
    dataDirectory_ = new QLineEdit(
        QString::fromStdString(context_->layout().dataRoot().string()), storageBox);
    dataDirectory_->setReadOnly(true);
    dataDirectory_->setToolTip(QStringLiteral(
        "Managed evidence storage and the case database live here. Changing the location while "
        "cases are open is not supported in Phase 0; set TRACE_DATA_DIR before starting TRACE."));
    thumbnailCache_ = new QLineEdit(
        QString::fromStdString(context_->settings().getString(settings_keys::kThumbnailCacheDirectory)),
        storageBox);
    thumbnailCache_->setPlaceholderText(
        QStringLiteral("Empty — previews are stored inside each case"));
    storageForm->addRow(QStringLiteral("Data directory"), dataDirectory_);
    storageForm->addRow(QStringLiteral("Thumbnail cache"), thumbnailCache_);
    layout->addWidget(storageBox);

    // --------------------------------------------------------------- playback
    auto* playbackBox = new QGroupBox(QStringLiteral("PLAYBACK"), this);
    auto* playbackForm = new QFormLayout(playbackBox);
    defaultSpeed_ = new QComboBox(playbackBox);
    for (const double speed : kPlaybackSpeeds) {
        defaultSpeed_->addItem(QStringLiteral("%1x").arg(speed, 0, 'g', 3), speed);
    }
    const double storedSpeed =
        context_->settings().getDouble(settings_keys::kPlaybackDefaultSpeed, 1.0);
    for (int i = 0; i < defaultSpeed_->count(); ++i) {
        if (qFuzzyCompare(defaultSpeed_->itemData(i).toDouble(), storedSpeed)) {
            defaultSpeed_->setCurrentIndex(i);
            break;
        }
    }
    jumpSeconds_ = new QSpinBox(playbackBox);
    jumpSeconds_->setRange(1, 300);
    jumpSeconds_->setSuffix(QStringLiteral(" s"));
    jumpSeconds_->setValue(
        static_cast<int>(context_->settings().getInt(settings_keys::kPlaybackJumpSeconds, 5)));
    volume_ = new QSpinBox(playbackBox);
    volume_->setRange(0, 100);
    volume_->setValue(static_cast<int>(context_->settings().getInt(settings_keys::kPlaybackVolume, 80)));
    playbackForm->addRow(QStringLiteral("Default speed"), defaultSpeed_);
    playbackForm->addRow(QStringLiteral("Jump interval"), jumpSeconds_);
    playbackForm->addRow(QStringLiteral("Volume"), volume_);
    playbackForm->addRow(QString(),
                         reservedNote(playbackBox, QStringLiteral(
                             "Audio plays at normal speed only. At any other speed the track is "
                             "silenced rather than pitch-shifted, because a pitch-shifted voice "
                             "misrepresents the recording. Volume and muting change what leaves "
                             "the sound card; neither alters the evidence.")));
    layout->addWidget(playbackBox);

    // -------------------------------------------------------- reserved: media
    auto* acceleration = new QGroupBox(QStringLiteral("ACCELERATION AND ANALYSIS"), this);
    auto* accelerationLayout = new QVBoxLayout(acceleration);
    auto* hardware = new QCheckBox(QStringLiteral("Use hardware-accelerated decoding"), acceleration);
    hardware->setChecked(context_->settings().getBool(settings_keys::kHardwareAcceleration, false));
    hardware->setEnabled(false);
    accelerationLayout->addWidget(hardware);
    accelerationLayout->addWidget(reservedNote(
        acceleration,
        QStringLiteral("Reserved. Phase 0 decodes in software so frame timing is identical on every "
                       "workstation. GPU decode and CUDA/TensorRT inference arrive with the "
                       "analysis phases.")));

    const auto providers = AnalysisProviderRegistry::instance().providers();
    auto* providerLabel = new QLabel(
        providers.empty()
            ? QStringLiteral("Analysis providers installed: none. Phase 0 ships the provider "
                             "interface only — no detection, tracking or recognition runs.")
            : QStringLiteral("Analysis providers installed: %1").arg(providers.size()),
        acceleration);
    providerLabel->setWordWrap(true);
    providerLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    accelerationLayout->addWidget(providerLabel);
    layout->addWidget(acceleration);

    // --------------------------------------------------------------- logging
    auto* loggingBox = new QGroupBox(QStringLiteral("LOGGING"), this);
    auto* loggingForm = new QFormLayout(loggingBox);
    logLevel_ = new QComboBox(loggingBox);
    for (const auto* level : {"trace", "debug", "info", "warn", "error"}) {
        logLevel_->addItem(QString::fromUtf8(level));
    }
    logLevel_->setCurrentText(
        QString::fromStdString(context_->settings().getString(settings_keys::kLogLevel, "info")));
    loggingForm->addRow(QStringLiteral("Level"), logLevel_);
    auto* logPath = new QLabel(
        QString::fromStdString(Logger::instance().logFilePath().string()), loggingBox);
    logPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    logPath->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colors::kTextSecondary.name()));
    loggingForm->addRow(QStringLiteral("Log file"), logPath);
    layout->addWidget(loggingBox);

    // ------------------------------------------------------------ capabilities
    auto* systemBox = new QGroupBox(QStringLiteral("WORKSTATION AND BUILD"), this);
    auto* systemForm = new QFormLayout(systemBox);
    const SystemCapabilities capabilities =
        detectSystemCapabilities(context_->layout().dataRoot().string());
    systemForm->addRow(QStringLiteral("Operating system"),
                       new QLabel(QString::fromStdString(capabilities.operatingSystem + " " +
                                                         capabilities.architecture),
                                  systemBox));
    systemForm->addRow(QStringLiteral("Logical cores"),
                       new QLabel(QString::number(capabilities.logicalCores), systemBox));
    systemForm->addRow(QStringLiteral("Memory"),
                       new QLabel(capabilities.totalMemoryBytes > 0
                                      ? fileSize(capabilities.totalMemoryBytes)
                                      : kNotAvailable,
                                  systemBox));
    systemForm->addRow(QStringLiteral("Data volume free"),
                       new QLabel(capabilities.dataDiskFreeBytes > 0
                                      ? QStringLiteral("%1 of %2")
                                            .arg(fileSize(capabilities.dataDiskFreeBytes),
                                                 fileSize(capabilities.dataDiskTotalBytes))
                                      : kNotAvailable,
                                  systemBox));
    systemForm->addRow(QStringLiteral("TRACE"),
                       new QLabel(QStringLiteral("%1 (%2)")
                                      .arg(QString::fromUtf8(kApplicationVersion),
                                           QString::fromUtf8(kPhase)),
                                  systemBox));
    systemForm->addRow(QStringLiteral("Qt"), new QLabel(QString::fromUtf8(qVersion()), systemBox));
    systemForm->addRow(QStringLiteral("SQLite"),
                       new QLabel(QString::fromUtf8(sqlite3_libversion()), systemBox));
    systemForm->addRow(QStringLiteral("FFmpeg"),
                       new QLabel(QString::fromStdString(ffmpegIdentifier()), systemBox));
    layout->addWidget(systemBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    styleAsPrimaryAction(buttons->button(QDialogButtonBox::Save));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SettingsDialog::save() {
    auto& settings = context_->settings();
    QStringList failures;

    const auto record = [&](const Status& status, const QString& what) {
        if (!status) failures << QStringLiteral("%1: %2").arg(
            what, QString::fromStdString(status.error().message()));
    };

    record(settings.setString(settings_keys::kThumbnailCacheDirectory,
                              thumbnailCache_->text().trimmed().toStdString()),
           QStringLiteral("Thumbnail cache"));
    record(settings.setDouble(settings_keys::kPlaybackDefaultSpeed,
                              defaultSpeed_->currentData().toDouble()),
           QStringLiteral("Default speed"));
    record(settings.setInt(settings_keys::kPlaybackJumpSeconds, jumpSeconds_->value()),
           QStringLiteral("Jump interval"));
    record(settings.setInt(settings_keys::kPlaybackVolume, volume_->value()),
           QStringLiteral("Volume"));
    record(settings.setString(settings_keys::kLogLevel, logLevel_->currentText().toStdString()),
           QStringLiteral("Logging level"));

    if (!failures.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Some settings were not saved"),
                             failures.join(QStringLiteral("\n")));
        return;
    }
    Logger::instance().setLevel(settings.logLevel());
    context_->notifyAuditChanged();
    accept();
}

}  // namespace trace::ui
