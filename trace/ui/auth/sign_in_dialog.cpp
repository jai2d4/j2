#include "ui/auth/sign_in_dialog.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/security/password.h"
#include "ui/app/application_context.h"
#include "ui/common/theme.h"

namespace trace::ui {

SignInDialog::Mode SignInDialog::modeFor(ApplicationContext* context) {
    auto needed = context->auth().needsFirstRunSetup();
    // A database that cannot be read is not treated as empty: offering to create
    // an administrator on a broken database would be the wrong thing to do with
    // an installation that may already hold accounts.
    if (needed && needed.value()) return Mode::FirstRunSetup;
    return Mode::SignIn;
}

SignInDialog::SignInDialog(ApplicationContext* context, Mode mode, QWidget* parent)
    : QDialog(parent), context_(context), mode_(mode) {
    setModal(true);
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);

    auto* heading = new QLabel(this);
    auto* explanation = new QLabel(this);
    explanation->setWordWrap(true);
    explanation->setStyleSheet(QStringLiteral("color: %1;").arg(colors::kTextSecondary.name()));

    switch (mode_) {
        case Mode::FirstRunSetup:
            setWindowTitle(QStringLiteral("TRACE — first run"));
            heading->setText(QStringLiteral("Create the administrator account"));
            explanation->setText(QStringLiteral(
                "This installation has no accounts yet. TRACE records who performed every "
                "action on a piece of evidence, so it needs to know who you are before it "
                "will open a case."));
            break;
        case Mode::SignIn:
            setWindowTitle(QStringLiteral("TRACE — sign in"));
            heading->setText(QStringLiteral("Sign in"));
            explanation->setText(QStringLiteral(
                "Your account name is recorded against everything you do in this case file."));
            break;
        case Mode::ChangePassword:
            setWindowTitle(QStringLiteral("TRACE — choose a new password"));
            heading->setText(QStringLiteral("Choose your own password"));
            explanation->setText(QStringLiteral(
                "This account is still using a password an administrator issued. Someone else "
                "knows it, so until you replace it your actions cannot be attributed to you "
                "alone."));
            break;
    }
    heading->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    layout->addWidget(heading);
    layout->addWidget(explanation);
    layout->addSpacing(8);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    if (mode_ != Mode::ChangePassword) {
        username_ = new QLineEdit(this);
        username_->setPlaceholderText(QStringLiteral("username"));
        form->addRow(QStringLiteral("Username"), username_);
    }

    if (mode_ == Mode::FirstRunSetup) {
        displayName_ = new QLineEdit(this);
        displayName_->setPlaceholderText(QStringLiteral("shown in the audit trail"));
        form->addRow(QStringLiteral("Full name"), displayName_);
    }

    password_ = new QLineEdit(this);
    password_->setEchoMode(QLineEdit::Password);
    form->addRow(mode_ == Mode::ChangePassword ? QStringLiteral("Current password")
                                               : QStringLiteral("Password"),
                 password_);

    if (mode_ != Mode::SignIn) {
        confirm_ = new QLineEdit(this);
        confirm_->setEchoMode(QLineEdit::Password);
        form->addRow(mode_ == Mode::ChangePassword ? QStringLiteral("New password")
                                                   : QStringLiteral("Confirm password"),
                     confirm_);
    }
    if (mode_ == Mode::ChangePassword) {
        // Typed twice because it cannot be seen. A typo here would otherwise
        // leave the operator holding a password nobody knows, on an account
        // whose whole point is being attributable to them.
        confirmNew_ = new QLineEdit(this);
        confirmNew_->setEchoMode(QLineEdit::Password);
        form->addRow(QStringLiteral("Confirm new password"), confirmNew_);
    }
    layout->addLayout(form);

    if (mode_ != Mode::SignIn) {
        auto* rule = new QLabel(
            QStringLiteral("At least %1 characters. Length is what makes a password hard to "
                           "guess — a long phrase you can remember beats a short one full of "
                           "symbols.")
                .arg(password::kMinimumLength),
            this);
        rule->setWordWrap(true);
        rule->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                .arg(colors::kTextSecondary.name()));
        layout->addWidget(rule);
    }

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(colors::kFailure.name()));
    errorLabel_->hide();
    layout->addWidget(errorLabel_);

    layout->addSpacing(4);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    if (mode_ != Mode::ChangePassword) {
        auto* quit = new QPushButton(QStringLiteral("Quit"), this);
        connect(quit, &QPushButton::clicked, this, &QDialog::reject);
        buttons->addWidget(quit);
    }
    submit_ = new QPushButton(mode_ == Mode::FirstRunSetup ? QStringLiteral("Create account")
                                                           : QStringLiteral("Continue"),
                              this);
    submit_->setDefault(true);
    connect(submit_, &QPushButton::clicked, this, &SignInDialog::submit);
    buttons->addWidget(submit_);
    layout->addLayout(buttons);

    if (username_ != nullptr) username_->setFocus();
    else password_->setFocus();
}

void SignInDialog::showError(const QString& message) {
    lastError_ = message;
    errorLabel_->setText(message);
    errorLabel_->show();
}

void SignInDialog::submit() {
    errorLabel_->hide();
    lastError_.clear();

    const QString password = password_->text();

    switch (mode_) {
        case Mode::FirstRunSetup: {
            const QString username = username_->text().trimmed();
            if (username.isEmpty()) {
                showError(QStringLiteral("Choose a username."));
                return;
            }
            if (password != confirm_->text()) {
                showError(QStringLiteral("The two passwords do not match."));
                return;
            }
            auto created = context_->auth().createFirstAdministrator(
                username.toStdString(), displayName_->text().trimmed().toStdString(),
                password.toStdString());
            if (!created) {
                showError(QString::fromStdString(created.error().message()));
                return;
            }
            // Straight into a session: the operator has just proved they know the
            // password by choosing it.
            auto signedIn =
                context_->auth().signIn(username.toStdString(), password.toStdString());
            if (!signedIn) {
                showError(QString::fromStdString(signedIn.error().message()));
                return;
            }
            accept();
            return;
        }

        case Mode::SignIn: {
            auto signedIn = context_->auth().signIn(username_->text().trimmed().toStdString(),
                                                    password.toStdString());
            if (!signedIn) {
                // Shown verbatim from the service, which words it identically for
                // an unknown username and a wrong password on purpose.
                showError(QString::fromStdString(signedIn.error().message()));
                password_->clear();
                password_->setFocus();
                return;
            }
            accept();
            return;
        }

        case Mode::ChangePassword: {
            if (confirm_->text().isEmpty()) {
                showError(QStringLiteral("Enter the new password."));
                return;
            }
            if (confirm_->text() != confirmNew_->text()) {
                showError(QStringLiteral("The two new passwords do not match."));
                return;
            }
            if (confirm_->text() == password_->text()) {
                showError(QStringLiteral(
                    "The new password must be different from the one you were issued."));
                return;
            }
            const auto& account = UserContext::current().account();
            auto status = context_->auth().changePassword(account.id, password.toStdString(),
                                                          confirm_->text().toStdString());
            if (!status) {
                showError(QString::fromStdString(status.error().message()));
                return;
            }
            accept();
            return;
        }
    }
}

}  // namespace trace::ui
