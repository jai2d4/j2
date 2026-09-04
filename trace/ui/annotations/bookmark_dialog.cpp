#include "ui/annotations/bookmark_dialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/common/time_utils.h"
#include "ui/common/theme.h"

namespace trace::ui {

BookmarkDialog::BookmarkDialog(qint64 timestampUs, std::optional<Bookmark> existing,
                               QWidget* parent)
    : QDialog(parent), timestampUs_(existing ? existing->timestampUs : timestampUs) {
    setWindowTitle(existing ? QStringLiteral("Edit bookmark") : QStringLiteral("Add bookmark"));
    setModal(true);
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto* heading = new QLabel(existing ? QStringLiteral("EDIT BOOKMARK")
                                        : QStringLiteral("ADD BOOKMARK"),
                               this);
    heading->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                          "letter-spacing: 1.5px;")
                               .arg(colors::kTextSecondary.name()));
    layout->addWidget(heading);

    auto* form = new QFormLayout();
    form->setSpacing(10);

    timecode_ = new QLineEdit(QString::fromStdString(formatTimecode(timestampUs_)), this);
    timecode_->setObjectName(QStringLiteral("timecodeField"));
    timecode_->setFont(monospaceFont());
    timecode_->setToolTip(QStringLiteral("HH:MM:SS.mmm"));
    title_ = new QLineEdit(this);
    title_->setObjectName(QStringLiteral("titleField"));
    note_ = new QPlainTextEdit(this);
    note_->setObjectName(QStringLiteral("noteField"));
    note_->setFixedHeight(90);

    if (existing) {
        title_->setText(QString::fromStdString(existing->title));
        note_->setPlainText(QString::fromStdString(existing->note));
    }

    form->addRow(QStringLiteral("Timecode"), timecode_);
    form->addRow(QStringLiteral("Title"), title_);
    form->addRow(QStringLiteral("Note"), note_);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save bookmark"));
    styleAsPrimaryAction(buttons->button(QDialogButtonBox::Ok));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &BookmarkDialog::submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    title_->setFocus();
}

QString BookmarkDialog::title() const { return title_->text().trimmed(); }
QString BookmarkDialog::note() const { return note_->toPlainText(); }

void BookmarkDialog::submit() {
    const auto parsed = parseTimecode(timecode_->text().toStdString());
    if (!parsed) {
        QMessageBox::warning(this, QStringLiteral("Timecode not understood"),
                             QStringLiteral("Enter a timecode as HH:MM:SS.mmm, MM:SS or seconds."));
        timecode_->setFocus();
        return;
    }
    timestampUs_ = *parsed;
    accept();
}

}  // namespace trace::ui
