#pragma once

#include <QDialog>

#include <optional>

#include "core/models/bookmark.h"

class QLineEdit;
class QPlainTextEdit;

namespace trace::ui {

/// Create or edit a bookmark. The timecode is editable so an analyst can type
/// an exact position rather than scrubbing to it.
class BookmarkDialog : public QDialog {
    Q_OBJECT

public:
    BookmarkDialog(qint64 timestampUs, std::optional<Bookmark> existing = std::nullopt,
                   QWidget* parent = nullptr);

    qint64 timestampUs() const { return timestampUs_; }
    QString title() const;
    QString note() const;

private:
    void submit();

    qint64 timestampUs_ = 0;
    QLineEdit* timecode_ = nullptr;
    QLineEdit* title_ = nullptr;
    QPlainTextEdit* note_ = nullptr;
};

}  // namespace trace::ui
