#pragma once

#include <QDialog>
#include <QString>

#include <filesystem>

#include "core/security/workspace_keys.h"
#include "core/services/workspace_service.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace trace::ui {

/// The prompt that stands in front of an encrypted workspace, before anything
/// is opened.
///
/// It cannot be the sign-in dialog, and the reason is structural rather than
/// stylistic: signing in reads the users table, the users table is inside the
/// encrypted database, and the database cannot be opened without the key this
/// dialog exists to obtain. So this comes first, works against the keyring
/// alone, and hands over to the ordinary sign-in once the workspace is open.
///
/// An operator sees two prompts on an encrypted workstation, and that is
/// honest rather than redundant — the first proves they can decrypt the
/// workspace, the second establishes who is accountable for what happens in it.
/// They are usually the same credential, and the setup mode below creates both
/// from one so it stays that way.
class UnlockWorkspaceDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode {
        Unlock,   ///< an encrypted workspace exists; produce its key
        SetUp,    ///< a new workspace; decide whether it is encrypted at all
    };

    UnlockWorkspaceDialog(std::filesystem::path dataRoot, WorkspaceState state, Mode mode,
                          QWidget* parent = nullptr);

    /// The credential that succeeded, so the caller can offer it to the sign-in
    /// that follows rather than asking for the same thing twice.
    QString username() const;
    QString password() const;

    /// False when the operator chose an unencrypted workspace in setup mode.
    bool encryptionChosen() const { return encryptionChosen_; }

    /// Unlocks `keys` with the credential this dialog accepted. Separate from
    /// the dialog's own attempt so the caller owns the key rather than the
    /// widget.
    Status applyTo(WorkspaceKeys& keys) const;

    // Accessors, so a test can drive the real dialog rather than a stand-in.
    QLineEdit* usernameField() const { return username_; }
    QLineEdit* passwordField() const { return password_; }
    QLineEdit* confirmField() const { return confirm_; }
    QCheckBox* encryptBox() const { return encrypt_; }
    QPushButton* submitButton() const { return submit_; }
    QString lastError() const { return lastError_; }

private slots:
    void submit();
    void encryptionToggled(bool on);

private:
    void showError(const QString& message);

    std::filesystem::path dataRoot_;
    WorkspaceState state_;
    Mode mode_ = Mode::Unlock;
    bool encryptionChosen_ = false;

    QLineEdit* username_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* confirm_ = nullptr;
    QCheckBox* encrypt_ = nullptr;
    QLabel* error_ = nullptr;
    QLabel* explanation_ = nullptr;
    QPushButton* submit_ = nullptr;
    QString lastError_;
};

}  // namespace trace::ui
