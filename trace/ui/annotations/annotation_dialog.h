#pragma once

#include <QDialog>

#include <optional>

#include "core/models/annotation.h"

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;

namespace trace::ui {

/// Create or edit a timestamped note. Ticking the range box turns a moment into
/// a span — the same record with an end time, which is how future region and
/// measurement annotations will be stored too.
class AnnotationDialog : public QDialog {
    Q_OBJECT

public:
    AnnotationDialog(qint64 startUs, std::optional<Annotation> existing = std::nullopt,
                     QWidget* parent = nullptr);

    qint64 startUs() const { return startUs_; }
    std::optional<qint64> endUs() const { return endUs_; }
    QString text() const;

private:
    void submit();

    qint64 startUs_ = 0;
    std::optional<qint64> endUs_;
    QLineEdit* start_ = nullptr;
    QLineEdit* end_ = nullptr;
    QCheckBox* isRange_ = nullptr;
    QPlainTextEdit* text_ = nullptr;
};

}  // namespace trace::ui
