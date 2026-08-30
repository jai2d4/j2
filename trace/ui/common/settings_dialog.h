#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QCheckBox;
class QSpinBox;

namespace trace::ui {

class ApplicationContext;

/// Application settings, plus what the workstation reports it can do.
///
/// Options that Phase 0 does not act on are shown, disabled, with the reason —
/// they are part of the settings architecture, not live controls.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(ApplicationContext* context, QWidget* parent = nullptr);

private:
    void save();

    ApplicationContext* context_ = nullptr;
    QLineEdit* dataDirectory_ = nullptr;
    QLineEdit* thumbnailCache_ = nullptr;
    QComboBox* logLevel_ = nullptr;
    QComboBox* defaultSpeed_ = nullptr;
    QSpinBox* jumpSeconds_ = nullptr;
    QSpinBox* volume_ = nullptr;
    QCheckBox* hardware_ = nullptr;
};

}  // namespace trace::ui
