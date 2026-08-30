#include "ui/auth/unlock_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/common/logging.h"
#include "core/security/password.h"
#include "ui/common/theme.h"

namespace trace::ui {
namespace {

constexpr const char* kComponent = "startup";

}  // namespace

UnlockWorkspaceDialog::UnlockWorkspaceDialog(std::filesystem::path dataRoot, WorkspaceState state,
                                             Mode mode, QWidget* parent)
    : QDialog(parent), dataRoot_(std::move(dataRoot)), state_(std::move(state)), mode_(mode) {
    setModal(true);
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);

    auto* heading = new QLabel(this);
    heading->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    explanation_ = new QLabel(this);
    explanation_->setWordWrap(true);
    explanation_->setStyleSheet(
        QStringLiteral("color: %1;").arg(colors::kTextSecondary.name()));

    if (mode_ == Mode::Unlock) {
        setWindowTitle(QStringLiteral("TRACE — unlock workspace"));
        heading->setText(QStringLiteral("Unlock this workspace"));
        explanation_->setText(QStringLiteral(
            "The case database and the evidence in this data directory are stored encrypted. "
            "Nothing can be opened until one of its operators provides the password that "
            "unwraps the key."));
    } else {
        setWindowTitle(QStringLiteral("TRACE — new workspace"));
        heading->setText(QStringLiteral("Set up this workspace"));
    }
    layout->addWidget(heading);
    layout->addWidget(explanation_);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    username_ = new QLineEdit(this);
    username_->setPlaceholderText(QStringLiteral("username"));
    password_ = new QLineEdit(this);
    password_->setEchoMode(QLineEdit::Password);
    confirm_ = new QLineEdit(this);
    confirm_->setEchoMode(QLineEdit::Password);

    if (mode_ == Mode::SetUp) {
        encrypt_ = new QCheckBox(QStringLiteral("Encrypt this workspace"), this);
        // On by default. A forensic workstation that holds recordings of people
        // should be the one that has to be argued out of encrypting them, not
        // into it.
        encrypt_->setChecked(state_.buildSupportsEncryption);
        encrypt_->setEnabled(state_.buildSupportsEncryption);
        layout->addWidget(encrypt_);
        connect(encrypt_, &QCheckBox::toggled, this, &UnlockWorkspaceDialog::encryptionToggled);
    }

    form->addRow(QStringLiteral("Operator"), username_);
    form->addRow(QStringLiteral("Password"), password_);
    if (mode_ == Mode::SetUp) form->addRow(QStringLiteral("Confirm password"), confirm_);
    layout->addLayout(form);

    error_ = new QLabel(this);
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: %1;").arg(colors::kFailure.name()));
    error_->hide();
    layout->addWidget(error_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    submit_ = buttons->addButton(mode_ == Mode::Unlock ? QStringLiteral("Unlock")
                                                       : QStringLiteral("Create workspace"),
                                 QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(submit_, &QPushButton::clicked, this, &UnlockWorkspaceDialog::submit);
    layout->addWidget(buttons);

    if (mode_ == Mode::SetUp) encryptionToggled(encrypt_->isChecked());
    username_->setFocus();
}

QString UnlockWorkspaceDialog::username() const { return username_->text().trimmed(); }
QString UnlockWorkspaceDialog::password() const { return password_->text(); }

void UnlockWorkspaceDialog::encryptionToggled(bool on) {
    encryptionChosen_ = on;
    if (!state_.buildSupportsEncryption) {
        explanation_->setText(QStringLiteral(
            "This build of TRACE was built without encryption support, so a workspace created "
            "here stores its case database and evidence as ordinary files. Anyone with the disk "
            "can read them."));
        return;
    }
    if (on) {
        // Stated before the workspace exists, because afterwards it is too late
        // to be useful. A forgotten password on an encrypted workspace is not a
        // recoverable situation and nobody should discover that later.
        explanation_->setText(QStringLiteral(
            "The case database and every recording ingested into this workspace will be stored "
            "encrypted. This password unwraps the key.\n\n"
            "There is no way to recover it. TRACE does not hold a copy, and neither does anyone "
            "else — if it is lost, so is the workspace. Other operators can be given their own "
            "access later, which is the way to make sure one forgotten password is not the end "
            "of a case file."));
    } else {
        explanation_->setText(QStringLiteral(
            "The case database and the evidence will be stored as ordinary files. Anyone with "
            "access to the disk can read them without going through TRACE. You can encrypt this "
            "workspace later from the File menu."));
    }
}

void UnlockWorkspaceDialog::showError(const QString& message) {
    lastError_ = message;
    error_->setText(message);
    error_->show();
}

Status UnlockWorkspaceDialog::applyTo(WorkspaceKeys& keys) const {
    return WorkspaceService::unlock(dataRoot_, username().toStdString(),
                                    password().toStdString(), keys);
}

void UnlockWorkspaceDialog::submit() {
    const QString user = username();
    const QString secret = password();

    if (user.isEmpty()) {
        showError(QStringLiteral("Enter the operator name this workspace was set up with."));
        return;
    }

    if (mode_ == Mode::SetUp) {
        if (!encryptionChosen_) {
            // Nothing to create: an unencrypted workspace is simply a data
            // directory, and the account is made by the sign-in that follows.
            accept();
            return;
        }
        if (secret != confirm_->text()) {
            showError(QStringLiteral("The two passwords do not match."));
            return;
        }
        if (auto strength = password::checkStrength(secret.toStdString()); !strength) {
            showError(QString::fromStdString(strength.error().message()));
            return;
        }
        auto created = WorkspaceService::createEncryptedWorkspace(
            dataRoot_, user.toStdString(), secret.toStdString());
        if (!created) {
            showError(QString::fromStdString(created.error().toString()));
            return;
        }
        logInfo(kComponent, "Encrypted workspace created");
        accept();
        return;
    }

    // Unlock: try it here so a wrong password is answered in this dialog rather
    // than by the application failing to start.
    WorkspaceKeys probe;
    if (auto status = applyTo(probe); !status) {
        showError(QString::fromStdString(status.error().message()));
        password_->clear();
        password_->setFocus();
        return;
    }
    accept();
}

}  // namespace trace::ui
