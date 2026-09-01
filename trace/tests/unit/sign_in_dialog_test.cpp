// The sign-in dialog — the gate in front of the application.
//
// Driven as a real widget rather than through mocks: the fields are filled and
// the button pressed, because what matters is whether the dialog admits the
// right person and refuses everybody else, not whether its methods can be
// called.
#include <gtest/gtest.h>

#include <QApplication>
#include <QLineEdit>
#include <QPushButton>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "core/common/uuid.h"
#include "core/security/user_context.h"
#include "ui/app/application_context.h"
#include "ui/auth/sign_in_dialog.h"

namespace trace {
namespace {

/// A data root that cleans up after itself, so each case starts with an
/// installation nobody has set up yet.
class ScratchRoot {
public:
    explicit ScratchRoot(const std::string& name) {
        // A UUID, not rand(). An unseeded rand() returns the same first value in
        // every process, so two test binaries running at once picked the same
        // directory and the second one found an administrator already created —
        // a failure that only appears under `ctest -j` and looks like a defect
        // in the code under test.
        path_ = std::filesystem::temp_directory_path() /
                ("trace-signin-" + name + "-" + uuidToCompact(generateUuid()).substr(0, 12));
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

struct Fixture {
    ScratchRoot root;
    ui::ApplicationContext context;

    explicit Fixture(const std::string& name) : root(name) {
        UserContext::current().clear();
        [&] { ASSERT_TRUE(context.initialise(root.path()).ok()); }();
    }
    ~Fixture() { context.shutdown(); }
};

TEST(SignInDialogTest, AnEmptyInstallationIsOfferedSetupRatherThanAPrompt) {
    Fixture fixture("mode-empty");
    EXPECT_EQ(ui::SignInDialog::modeFor(&fixture.context),
              ui::SignInDialog::Mode::FirstRunSetup)
        << "a sign-in prompt on an installation with no account is one nobody can satisfy";
}

TEST(SignInDialogTest, SetupCreatesTheAdministratorAndSignsThemIn) {
    Fixture fixture("setup");
    ui::SignInDialog dialog(&fixture.context, ui::SignInDialog::Mode::FirstRunSetup);

    dialog.usernameField()->setText(QStringLiteral("analyst"));
    dialog.passwordField()->setText(QStringLiteral("correct horse battery staple"));
    dialog.confirmField()->setText(QStringLiteral("correct horse battery staple"));
    dialog.submitButton()->click();

    EXPECT_EQ(dialog.result(), QDialog::Accepted) << dialog.lastError().toStdString();
    EXPECT_EQ(UserContext::current().actorName(), "analyst");
    EXPECT_TRUE(UserContext::current().authenticated());

    // And the installation is no longer one that needs setting up.
    EXPECT_EQ(ui::SignInDialog::modeFor(&fixture.context), ui::SignInDialog::Mode::SignIn);
}

TEST(SignInDialogTest, SetupRefusesTwoPasswordsThatDoNotMatch) {
    Fixture fixture("mismatch");
    ui::SignInDialog dialog(&fixture.context, ui::SignInDialog::Mode::FirstRunSetup);

    dialog.usernameField()->setText(QStringLiteral("analyst"));
    dialog.passwordField()->setText(QStringLiteral("correct horse battery staple"));
    dialog.confirmField()->setText(QStringLiteral("correct horse battery stapl"));
    dialog.submitButton()->click();

    EXPECT_NE(dialog.result(), QDialog::Accepted);
    EXPECT_FALSE(dialog.lastError().isEmpty());
    EXPECT_FALSE(UserContext::current().authenticated())
        << "a rejected setup must not leave somebody signed in";
}

TEST(SignInDialogTest, SetupRefusesAPasswordTooShortToBeWorthHaving) {
    Fixture fixture("weak");
    ui::SignInDialog dialog(&fixture.context, ui::SignInDialog::Mode::FirstRunSetup);

    dialog.usernameField()->setText(QStringLiteral("analyst"));
    dialog.passwordField()->setText(QStringLiteral("short"));
    dialog.confirmField()->setText(QStringLiteral("short"));
    dialog.submitButton()->click();

    EXPECT_NE(dialog.result(), QDialog::Accepted);
    EXPECT_FALSE(dialog.lastError().isEmpty());
    // The account was not created as a side effect of the rejected attempt.
    EXPECT_EQ(ui::SignInDialog::modeFor(&fixture.context),
              ui::SignInDialog::Mode::FirstRunSetup);
}

TEST(SignInDialogTest, TheRightCredentialIsAdmittedAndTheWrongOneIsNot) {
    Fixture fixture("signin");
    ASSERT_TRUE(fixture.context.auth()
                    .createFirstAdministrator("analyst", "A. Analyst",
                                              "correct horse battery staple")
                    .ok());
    UserContext::current().clear();

    {
        ui::SignInDialog wrong(&fixture.context, ui::SignInDialog::Mode::SignIn);
        wrong.usernameField()->setText(QStringLiteral("analyst"));
        wrong.passwordField()->setText(QStringLiteral("not the password"));
        wrong.submitButton()->click();

        EXPECT_NE(wrong.result(), QDialog::Accepted);
        EXPECT_FALSE(wrong.lastError().isEmpty());
        EXPECT_FALSE(UserContext::current().authenticated());
        // The field is cleared so the next attempt starts from nothing rather
        // than from a wrong guess the operator has to notice and delete.
        EXPECT_TRUE(wrong.passwordField()->text().isEmpty());
    }
    {
        ui::SignInDialog right(&fixture.context, ui::SignInDialog::Mode::SignIn);
        right.usernameField()->setText(QStringLiteral("analyst"));
        right.passwordField()->setText(QStringLiteral("correct horse battery staple"));
        right.submitButton()->click();

        EXPECT_EQ(right.result(), QDialog::Accepted) << right.lastError().toStdString();
        EXPECT_TRUE(UserContext::current().authenticated());
    }
}

TEST(SignInDialogTest, TheDialogNeverSaysWhetherTheUsernameExists) {
    Fixture fixture("enumeration");
    ASSERT_TRUE(fixture.context.auth()
                    .createFirstAdministrator("analyst", "A. Analyst",
                                              "correct horse battery staple")
                    .ok());
    UserContext::current().clear();

    ui::SignInDialog wrongPassword(&fixture.context, ui::SignInDialog::Mode::SignIn);
    wrongPassword.usernameField()->setText(QStringLiteral("analyst"));
    wrongPassword.passwordField()->setText(QStringLiteral("not the password"));
    wrongPassword.submitButton()->click();

    ui::SignInDialog unknownUser(&fixture.context, ui::SignInDialog::Mode::SignIn);
    unknownUser.usernameField()->setText(QStringLiteral("nobody"));
    unknownUser.passwordField()->setText(QStringLiteral("not the password"));
    unknownUser.submitButton()->click();

    // The service words these identically on purpose; the dialog must show what
    // it was given rather than helpfully distinguishing them.
    EXPECT_EQ(wrongPassword.lastError(), unknownUser.lastError());
    EXPECT_FALSE(wrongPassword.lastError().isEmpty());
}

TEST(SignInDialogTest, ChangingAnIssuedPasswordRequiresTheCurrentOneAndTwoMatchingNewOnes) {
    Fixture fixture("change");
    ASSERT_TRUE(fixture.context.auth()
                    .createFirstAdministrator("admin", "Admin", "correct horse battery staple")
                    .ok());
    ASSERT_TRUE(fixture.context.auth().signIn("admin", "correct horse battery staple").ok());
    ASSERT_TRUE(fixture.context.auth()
                    .createAccount("analyst", "A. Analyst", UserRole::Analyst,
                                   "issued temporary password")
                    .ok());
    fixture.context.auth().signOut();
    ASSERT_TRUE(fixture.context.auth().signIn("analyst", "issued temporary password").ok());
    ASSERT_TRUE(fixture.context.auth().mustChangePassword());

    {
        // The two new entries disagree — a typo the operator cannot see.
        ui::SignInDialog typo(&fixture.context, ui::SignInDialog::Mode::ChangePassword);
        typo.passwordField()->setText(QStringLiteral("issued temporary password"));
        typo.confirmField()->setText(QStringLiteral("my own long password"));
        typo.confirmNewField()->setText(QStringLiteral("my own long passwrod"));
        typo.submitButton()->click();
        EXPECT_NE(typo.result(), QDialog::Accepted);
        EXPECT_TRUE(fixture.context.auth().mustChangePassword());
    }
    {
        ui::SignInDialog good(&fixture.context, ui::SignInDialog::Mode::ChangePassword);
        good.passwordField()->setText(QStringLiteral("issued temporary password"));
        good.confirmField()->setText(QStringLiteral("my own long password"));
        good.confirmNewField()->setText(QStringLiteral("my own long password"));
        good.submitButton()->click();
        EXPECT_EQ(good.result(), QDialog::Accepted) << good.lastError().toStdString();
        EXPECT_FALSE(fixture.context.auth().mustChangePassword());
    }
}

}  // namespace
}  // namespace trace

int main(int argc, char** argv) {
    if (std::getenv("QT_QPA_PLATFORM") == nullptr) {
#if defined(_WIN32)
        _putenv_s("QT_QPA_PLATFORM", "offscreen");
#else
        setenv("QT_QPA_PLATFORM", "offscreen", 0);
#endif
    }
    QApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
