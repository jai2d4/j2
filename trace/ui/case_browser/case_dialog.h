#pragma once

#include <QDialog>

#include <optional>

#include "core/models/case.h"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace trace::ui {

class ApplicationContext;

/// Create or edit a case. The case number is pre-filled with the next unused
/// CASE-nnnn but stays editable so an agency reference can be used instead.
class CaseDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode { Create, Edit };

    CaseDialog(ApplicationContext* context, Mode mode, std::optional<Case> existing = std::nullopt,
               QWidget* parent = nullptr);

    /// Populated after the dialog is accepted.
    const std::optional<Case>& result() const { return result_; }

private:
    void submit();

    ApplicationContext* context_ = nullptr;
    Mode mode_;
    std::optional<Case> existing_;
    std::optional<Case> result_;

    QLineEdit* caseNumber_ = nullptr;
    QLineEdit* title_ = nullptr;
    QPlainTextEdit* description_ = nullptr;
    QComboBox* status_ = nullptr;
    QLineEdit* investigator_ = nullptr;
    QLineEdit* agency_ = nullptr;
    QLineEdit* location_ = nullptr;
    QPlainTextEdit* notes_ = nullptr;
};

}  // namespace trace::ui
