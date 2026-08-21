#include "ui/annotations/annotation_dialog.h"

#include <QCheckBox>
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

AnnotationDialog::AnnotationDialog(qint64 startUs, std::optional<Annotation> existing,
                                   QWidget* parent)
    : QDialog(parent), startUs_(existing ? existing->startUs : startUs) {
    setWindowTitle(existing ? QStringLiteral("Edit note") : QStringLiteral("Add note"));
    setModal(true);
    setMinimumWidth(480);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto* heading = new QLabel(existing ? QStringLiteral("EDIT NOTE") : QStringLiteral("ADD NOTE"),
                               this);
    heading->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                          "letter-spacing: 1.5px;")
                               .arg(colors::kTextSecondary.name()));
    layout->addWidget(heading);

    auto* form = new QFormLayout();
    form->setSpacing(10);

    start_ = new QLineEdit(QString::fromStdString(formatTimecode(startUs_)), this);
    start_->setObjectName(QStringLiteral("startField"));
    start_->setFont(monospaceFont());
    isRange_ = new QCheckBox(QStringLiteral("This note covers a time range"), this);
    isRange_->setObjectName(QStringLiteral("rangeCheck"));
    end_ = new QLineEdit(this);
    end_->setObjectName(QStringLiteral("endField"));
    end_->setFont(monospaceFont());
    end_->setEnabled(false);
    text_ = new QPlainTextEdit(this);
    text_->setObjectName(QStringLiteral("textField"));
    text_->setFixedHeight(120);

    if (existing) {
        text_->setPlainText(QString::fromStdString(existing->text));
        if (existing->endUs) {
            isRange_->setChecked(true);
            end_->setEnabled(true);
            end_->setText(QString::fromStdString(formatTimecode(*existing->endUs)));
        }
    }
    if (!isRange_->isChecked()) {
        end_->setText(QString::fromStdString(formatTimecode(startUs_ + 5'000'000)));
    }

    form->addRow(QStringLiteral("Start"), start_);
    form->addRow(QString(), isRange_);
    form->addRow(QStringLiteral("End"), end_);
    form->addRow(QStringLiteral("Note"), text_);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save note"));
    styleAsPrimaryAction(buttons->button(QDialogButtonBox::Ok));
    layout->addWidget(buttons);

    connect(isRange_, &QCheckBox::toggled, end_, &QLineEdit::setEnabled);
    connect(buttons, &QDialogButtonBox::accepted, this, &AnnotationDialog::submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    text_->setFocus();
}

QString AnnotationDialog::text() const { return text_->toPlainText().trimmed(); }

void AnnotationDialog::submit() {
    const auto parsedStart = parseTimecode(start_->text().toStdString());
    if (!parsedStart) {
        QMessageBox::warning(this, QStringLiteral("Timecode not understood"),
                             QStringLiteral("Enter a start time as HH:MM:SS.mmm, MM:SS or seconds."));
        return;
    }
    startUs_ = *parsedStart;

    if (isRange_->isChecked()) {
        const auto parsedEnd = parseTimecode(end_->text().toStdString());
        if (!parsedEnd) {
            QMessageBox::warning(this, QStringLiteral("Timecode not understood"),
                                 QStringLiteral("Enter an end time as HH:MM:SS.mmm, MM:SS or seconds."));
            return;
        }
        if (*parsedEnd < startUs_) {
            QMessageBox::warning(this, QStringLiteral("Range is inverted"),
                                 QStringLiteral("The end of a note cannot be before its start."));
            return;
        }
        endUs_ = *parsedEnd;
    } else {
        endUs_.reset();
    }

    if (text().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Note is empty"),
                             QStringLiteral("Enter the observation this note records."));
        return;
    }
    accept();
}

}  // namespace trace::ui
