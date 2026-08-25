#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;

namespace trace::ui {

class ApplicationContext;

/// The gate in front of the application.
///
/// One dialog covers three moments because they are the same conversation: the
/// very first launch, when there is nobody to sign in as and an administrator
/// has to be created; an ordinary sign-in; and the forced change when an account
/// is still holding a password somebody else chose.
///
/// Nothing in this class writes a password anywhere. The plaintext lives in the
/// QLineEdit and is passed to AuthService; it is not logged, not held in a
/// member, and not put in a message.
class SignInDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode {
        SignIn,           ///< an account exists; prove you are it
        FirstRunSetup,    ///< no account exists; create the first administrator
        ChangePassword,   ///< signed in, but on a password somebody else issued
    };

    SignInDialog(ApplicationContext* context, Mode mode, QWidget* parent = nullptr);

    /// Chooses the mode from the state of the database, so a caller cannot show
    /// a sign-in prompt to an empty installation or setup to a populated one.
    static Mode modeFor(ApplicationContext* context);

    // Accessors used by the acceptance test to drive the real dialog.
    QLineEdit* usernameField() const { return username_; }
    QLineEdit* passwordField() const { return password_; }
    QLineEdit* confirmField() const { return confirm_; }
    QPushButton* submitButton() const { return submit_; }
    QString lastError() const { return lastError_; }

private slots:
    void submit();

private:
    void showError(const QString& message);

    ApplicationContext* context_ = nullptr;
    Mode mode_ = Mode::SignIn;

    QLineEdit* username_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* confirm_ = nullptr;
    QLineEdit* confirmNew_ = nullptr;
    QLineEdit* displayName_ = nullptr;
    QPushButton* submit_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QString lastError_;
};

}  // namespace trace::ui
