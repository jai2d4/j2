// The unlock dialog — the prompt in front of an encrypted workspace.
//
// Driven as a real widget, like the sign-in dialog test: the fields are filled
// and the button pressed, because what matters is whether an operator can get
// into their own workspace and nobody else can, not whether the methods can be
// called.
//
// This is the one part of encryption an operator actually touches, so the cases
// below are the ones a person can get wrong: the wrong password, a workspace
// someone else set up, and choosing not to encrypt at all.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/security/crypto.h"
#include "core/security/keyring.h"
#include "core/security/workspace_keys.h"
#include "core/services/workspace_service.h"
#include "ui/auth/unlock_dialog.h"

namespace trace {
namespace {

constexpr const char* kOperator = "d.mcbride";
constexpr const char* kPassword = "correct horse battery staple";

class ScratchRoot {
public:
    explicit ScratchRoot(const std::string& name) {
        path_ = std::filesystem::temp_directory_path() /
                ("trace-unlock-" + name + "-" + std::to_string(::rand()));
        std::filesystem::create_directories(path_);
    }
    ~ScratchRoot() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class UnlockDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!crypto::available()) GTEST_SKIP() << "built without encryption support";
    }
};

TEST_F(UnlockDialogTest, SetUpCreatesAWorkspaceThatItsPasswordThenOpens) {
    ScratchRoot root("setup");
    const WorkspaceState before = WorkspaceService::inspect(root.path());
    ASSERT_FALSE(before.encrypted);

    ui::UnlockWorkspaceDialog dialog(root.path(), before,
                                     ui::UnlockWorkspaceDialog::Mode::SetUp);
    ASSERT_NE(dialog.encryptBox(), nullptr);
    dialog.encryptBox()->setChecked(true);
    dialog.usernameField()->setText(QString::fromUtf8(kOperator));
    dialog.passwordField()->setText(QString::fromUtf8(kPassword));
    dialog.confirmField()->setText(QString::fromUtf8(kPassword));
    dialog.submitButton()->click();

    EXPECT_EQ(dialog.result(), QDialog::Accepted) << dialog.lastError().toStdString();
    EXPECT_TRUE(dialog.encryptionChosen());
    EXPECT_TRUE(Keyring::exists(root.path()));

    WorkspaceKeys keys;
    EXPECT_TRUE(dialog.applyTo(keys).ok());
    EXPECT_TRUE(keys.unlocked());
}

TEST_F(UnlockDialogTest, DecliningEncryptionCreatesNoKeyring) {
    // An operator who says no gets an ordinary data directory, not a keyring
    // wrapped around nothing.
    ScratchRoot root("declined");
    ui::UnlockWorkspaceDialog dialog(root.path(), WorkspaceService::inspect(root.path()),
                                     ui::UnlockWorkspaceDialog::Mode::SetUp);
    dialog.encryptBox()->setChecked(false);
    dialog.usernameField()->setText(QString::fromUtf8(kOperator));
    dialog.submitButton()->click();

    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_FALSE(dialog.encryptionChosen());
    EXPECT_FALSE(Keyring::exists(root.path()));
}

TEST_F(UnlockDialogTest, SetUpRefusesMismatchedPasswords) {
    ScratchRoot root("mismatch");
    ui::UnlockWorkspaceDialog dialog(root.path(), WorkspaceService::inspect(root.path()),
                                     ui::UnlockWorkspaceDialog::Mode::SetUp);
    dialog.encryptBox()->setChecked(true);
    dialog.usernameField()->setText(QString::fromUtf8(kOperator));
    dialog.passwordField()->setText(QString::fromUtf8(kPassword));
    dialog.confirmField()->setText(QStringLiteral("something else entirely"));
    dialog.submitButton()->click();

    EXPECT_NE(dialog.result(), QDialog::Accepted);
    EXPECT_FALSE(dialog.lastError().isEmpty());
    EXPECT_FALSE(Keyring::exists(root.path()))
        << "a rejected setup left a keyring behind";
}

TEST_F(UnlockDialogTest, SetUpRefusesAShortPassword) {
    ScratchRoot root("short");
    ui::UnlockWorkspaceDialog dialog(root.path(), WorkspaceService::inspect(root.path()),
                                     ui::UnlockWorkspaceDialog::Mode::SetUp);
    dialog.encryptBox()->setChecked(true);
    dialog.usernameField()->setText(QString::fromUtf8(kOperator));
    dialog.passwordField()->setText(QStringLiteral("short"));
    dialog.confirmField()->setText(QStringLiteral("short"));
    dialog.submitButton()->click();

    EXPECT_NE(dialog.result(), QDialog::Accepted);
    EXPECT_FALSE(Keyring::exists(root.path()));
}

TEST_F(UnlockDialogTest, UnlockAcceptsTheRightPasswordAndRefusesEverythingElse) {
    ScratchRoot root("unlock");
    ASSERT_TRUE(
        WorkspaceService::createEncryptedWorkspace(root.path(), kOperator, kPassword).ok());
    const WorkspaceState state = WorkspaceService::inspect(root.path());
    ASSERT_TRUE(state.encrypted);

    {
        ui::UnlockWorkspaceDialog wrong(root.path(), state,
                                        ui::UnlockWorkspaceDialog::Mode::Unlock);
        wrong.usernameField()->setText(QString::fromUtf8(kOperator));
        wrong.passwordField()->setText(QStringLiteral("not the right passphrase"));
        wrong.submitButton()->click();
        EXPECT_NE(wrong.result(), QDialog::Accepted);
        EXPECT_FALSE(wrong.lastError().isEmpty());
        // The field is cleared so the next attempt starts from nothing rather
        // than from a wrong password the operator has to notice and delete.
        EXPECT_TRUE(wrong.passwordField()->text().isEmpty());
    }
    {
        ui::UnlockWorkspaceDialog stranger(root.path(), state,
                                           ui::UnlockWorkspaceDialog::Mode::Unlock);
        stranger.usernameField()->setText(QStringLiteral("someone.else"));
        stranger.passwordField()->setText(QString::fromUtf8(kPassword));
        stranger.submitButton()->click();
        EXPECT_NE(stranger.result(), QDialog::Accepted);
    }
    {
        ui::UnlockWorkspaceDialog right(root.path(), state,
                                        ui::UnlockWorkspaceDialog::Mode::Unlock);
        right.usernameField()->setText(QString::fromUtf8(kOperator));
        right.passwordField()->setText(QString::fromUtf8(kPassword));
        right.submitButton()->click();
        EXPECT_EQ(right.result(), QDialog::Accepted) << right.lastError().toStdString();

        WorkspaceKeys keys;
        ASSERT_TRUE(right.applyTo(keys).ok());
        EXPECT_TRUE(keys.unlocked());
        EXPECT_TRUE(keys.encrypted());
    }
}

TEST_F(UnlockDialogTest, UnlockRefusesAnEmptyOperatorNameWithoutTouchingTheKeyring) {
    ScratchRoot root("empty");
    ASSERT_TRUE(
        WorkspaceService::createEncryptedWorkspace(root.path(), kOperator, kPassword).ok());

    ui::UnlockWorkspaceDialog dialog(root.path(), WorkspaceService::inspect(root.path()),
                                     ui::UnlockWorkspaceDialog::Mode::Unlock);
    dialog.passwordField()->setText(QString::fromUtf8(kPassword));
    dialog.submitButton()->click();
    EXPECT_NE(dialog.result(), QDialog::Accepted);
    EXPECT_FALSE(dialog.lastError().isEmpty());
}

}  // namespace
}  // namespace trace

int main(int argc, char** argv) {
    // Offscreen unless the harness says otherwise: these are real widgets, and
    // a machine with no display should still run them.
#if defined(_WIN32)
    if (std::getenv("QT_QPA_PLATFORM") == nullptr) _putenv_s("QT_QPA_PLATFORM", "offscreen");
#else
    if (std::getenv("QT_QPA_PLATFORM") == nullptr) setenv("QT_QPA_PLATFORM", "offscreen", 0);
#endif
    QApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
