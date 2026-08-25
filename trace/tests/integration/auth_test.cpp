// Local accounts: sign-in, lockout, and the rules that keep an audit trail
// attributable to a person.
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "core/common/uuid.h"
#include "core/repositories/user_repository.h"
#include "core/security/user_context.h"
#include "core/services/auth_service.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

constexpr const char* kGoodPassword = "correct horse battery staple";

struct AuthFixture {
    testing::TemporaryDirectory dataRoot;
    testing::TestStack stack;
    std::unique_ptr<AuthService> auth;

    explicit AuthFixture(const std::string& prefix)
        : dataRoot(prefix), stack(testing::TestStack::create(dataRoot.path())) {
        auth = std::make_unique<AuthService>(stack.database, stack.audit);
        // Each test starts with nobody signed in, as the application does.
        UserContext::current().setAccount(UserAccount{});
    }

    Result<std::vector<AuditEvent>> auditEvents() { return stack.audit->list(); }

    bool auditContains(AuditAction action) {
        auto events = auditEvents();
        if (!events) return false;
        for (const auto& event : events.value()) {
            if (event.action == action) return true;
        }
        return false;
    }
};

TEST(AuthTest, AFreshInstallationHasNobodyToSignInAs) {
    AuthFixture fixture("trace-auth-firstrun");

    auto needed = fixture.auth->needsFirstRunSetup();
    ASSERT_TRUE(needed.ok());
    EXPECT_TRUE(needed.value())
        << "an installation with no credential must ask for setup, not offer a sign-in prompt "
           "nobody can satisfy";

    auto created = fixture.auth->createFirstAdministrator("analyst", "A. Analyst", kGoodPassword);
    ASSERT_TRUE(created.ok()) << created.error().message();
    EXPECT_EQ(created.value().role, UserRole::Administrator);

    auto after = fixture.auth->needsFirstRunSetup();
    ASSERT_TRUE(after.ok());
    EXPECT_FALSE(after.value());
}

TEST(AuthTest, TheUnauthenticatedSetupPathClosesBehindItself) {
    AuthFixture fixture("trace-auth-setup-once");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("first", "First", kGoodPassword).ok());

    // This is the one path that mints an administrator without anybody proving
    // who they are. Once an account exists it must refuse, or it is a permanent
    // back door.
    auto second =
        fixture.auth->createFirstAdministrator("second", "Second", "another good password");
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(second.error().code(), ErrorCode::PermissionDenied);
}

TEST(AuthTest, SigningInEstablishesTheIdentityTheAuditTrailWillName) {
    AuthFixture fixture("trace-auth-signin");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("analyst", "A. Analyst", kGoodPassword).ok());

    auto signedIn = fixture.auth->signIn("analyst", kGoodPassword);
    ASSERT_TRUE(signedIn.ok()) << signedIn.error().message();
    EXPECT_EQ(signedIn.value().username, "analyst");
    EXPECT_EQ(UserContext::current().actorName(), "analyst");
    EXPECT_TRUE(fixture.auditContains(AuditAction::SignInSucceeded));

    fixture.auth->signOut();
    EXPECT_TRUE(UserContext::current().actorName().empty())
        << "signing out must clear the identity, or the next action is attributed to somebody "
           "who has left";
    EXPECT_TRUE(fixture.auditContains(AuditAction::SignedOut));
}

TEST(AuthTest, AWrongPasswordAndAnUnknownUsernameAreIndistinguishable) {
    AuthFixture fixture("trace-auth-enumeration");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("analyst", "A. Analyst", kGoodPassword).ok());

    auto wrongPassword = fixture.auth->signIn("analyst", "not the password");
    auto unknownUser = fixture.auth->signIn("nobody", "not the password");

    ASSERT_FALSE(wrongPassword.ok());
    ASSERT_FALSE(unknownUser.ok());
    EXPECT_EQ(wrongPassword.error().message(), unknownUser.error().message())
        << "a different message for an unknown account turns the sign-in prompt into a "
           "directory of who holds one";
    EXPECT_EQ(wrongPassword.error().code(), unknownUser.error().code());
}

TEST(AuthTest, EveryAttemptIsRecordedWithoutRecordingThePassword) {
    AuthFixture fixture("trace-auth-audit");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("analyst", "A. Analyst", kGoodPassword).ok());

    const std::string secret = "a-very-distinctive-secret-value";
    fixture.auth->signIn("analyst", secret);
    EXPECT_TRUE(fixture.auditContains(AuditAction::SignInFailed))
        << "a failed attempt is exactly what an audit trail exists to show";

    // The password must not appear anywhere in the trail — not as plaintext, and
    // not tucked into a description or a details field.
    auto events = fixture.auditEvents();
    ASSERT_TRUE(events.ok());
    for (const auto& event : events.value()) {
        EXPECT_EQ(event.description.find(secret), std::string::npos)
            << "the attempted password appeared in an audit description";
        EXPECT_EQ(event.detailsJson.find(secret), std::string::npos)
            << "the attempted password appeared in audit details";
    }
}

TEST(AuthTest, RepeatedFailuresLockTheAccountEvenAgainstTheRightPassword) {
    AuthFixture fixture("trace-auth-lockout");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("analyst", "A. Analyst", kGoodPassword).ok());

    for (int attempt = 0; attempt < AuthService::kMaxFailedAttempts; ++attempt) {
        auto rejected = fixture.auth->signIn("analyst", "wrong");
        EXPECT_FALSE(rejected.ok());
    }

    // The correct password now fails too. Without this an attacker simply keeps
    // guessing; the work factor slows an offline attack, this stops an online one.
    auto locked = fixture.auth->signIn("analyst", kGoodPassword);
    ASSERT_FALSE(locked.ok());
    EXPECT_EQ(locked.error().code(), ErrorCode::PermissionDenied);
    EXPECT_NE(locked.error().message().find("locked"), std::string::npos)
        << "an operator locked out of their own case needs to be told why";
    EXPECT_TRUE(fixture.auditContains(AuditAction::AccountLocked));
}

TEST(AuthTest, ASuccessfulSignInClearsTheFailureCount) {
    AuthFixture fixture("trace-auth-counter");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("analyst", "A. Analyst", kGoodPassword).ok());

    // Short of the threshold, then a success: an operator who mistypes twice and
    // then gets it right must not be one attempt from a lockout tomorrow.
    for (int attempt = 0; attempt < AuthService::kMaxFailedAttempts - 1; ++attempt) {
        fixture.auth->signIn("analyst", "wrong");
    }
    ASSERT_TRUE(fixture.auth->signIn("analyst", kGoodPassword).ok());

    UserRepository users(fixture.stack.database);
    auto stored = users.findStoredByUsername("analyst");
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    EXPECT_EQ(stored.value()->failedAttempts, 0);
    EXPECT_FALSE(stored.value()->lockedUntil.has_value());
}

TEST(AuthTest, AnIssuedPasswordMustBeReplacedBeforeItAttributesAnything) {
    AuthFixture fixture("trace-auth-mustchange");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("admin", "Admin", kGoodPassword).ok());
    ASSERT_TRUE(fixture.auth->signIn("admin", kGoodPassword).ok());

    auto created = fixture.auth->createAccount("analyst", "A. Analyst", UserRole::Analyst,
                                               "issued temporary password");
    ASSERT_TRUE(created.ok()) << created.error().message();

    // The administrator knows this password, so an action taken under it is not
    // attributable to the analyst alone.
    ASSERT_TRUE(fixture.auth->signIn("analyst", "issued temporary password").ok());
    EXPECT_TRUE(fixture.auth->mustChangePassword());

    auto changed = fixture.auth->changePassword(UserContext::current().account().id,
                                                "issued temporary password", "my own long password");
    ASSERT_TRUE(changed.ok()) << changed.error().message();
    EXPECT_FALSE(fixture.auth->mustChangePassword());

    // And the old one no longer works.
    fixture.auth->signOut();
    EXPECT_FALSE(fixture.auth->signIn("analyst", "issued temporary password").ok());
    EXPECT_TRUE(fixture.auth->signIn("analyst", "my own long password").ok());
}

TEST(AuthTest, ChangingAPasswordRequiresTheCurrentOne) {
    AuthFixture fixture("trace-auth-change");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("analyst", "A. Analyst", kGoodPassword).ok());
    ASSERT_TRUE(fixture.auth->signIn("analyst", kGoodPassword).ok());

    // An unattended unlocked workstation must not be a permanent account takeover.
    auto refused = fixture.auth->changePassword(UserContext::current().account().id,
                                                "not the current password", "a new long password");
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);

    // The original still works, so the failed attempt changed nothing.
    fixture.auth->signOut();
    EXPECT_TRUE(fixture.auth->signIn("analyst", kGoodPassword).ok());
}

TEST(AuthTest, AWeakPasswordIsRefusedWhereverItIsOffered) {
    AuthFixture fixture("trace-auth-weak");

    auto tooShort = fixture.auth->createFirstAdministrator("analyst", "A. Analyst", "short");
    ASSERT_FALSE(tooShort.ok());
    EXPECT_EQ(tooShort.error().code(), ErrorCode::InvalidArgument);

    // And the account was not created as a side effect of the attempt.
    auto needed = fixture.auth->needsFirstRunSetup();
    ASSERT_TRUE(needed.ok());
    EXPECT_TRUE(needed.value());
}

TEST(AuthTest, AnInactiveAccountCannotSignIn) {
    AuthFixture fixture("trace-auth-inactive");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("admin", "Admin", kGoodPassword).ok());
    ASSERT_TRUE(fixture.auth->signIn("admin", kGoodPassword).ok());

    auto created = fixture.auth->createAccount("analyst", "A. Analyst", UserRole::Analyst,
                                               "issued temporary password");
    ASSERT_TRUE(created.ok());
    ASSERT_TRUE(fixture.auth->setActive(created.value().id, false).ok());

    auto rejected = fixture.auth->signIn("analyst", "issued temporary password");
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code(), ErrorCode::PermissionDenied);
}

TEST(AuthTest, ManagingAccountsRequiresTheAdministratorRole) {
    AuthFixture fixture("trace-auth-permission");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("admin", "Admin", kGoodPassword).ok());
    ASSERT_TRUE(fixture.auth->signIn("admin", kGoodPassword).ok());
    auto analyst = fixture.auth->createAccount("analyst", "A. Analyst", UserRole::Analyst,
                                               "issued temporary password");
    ASSERT_TRUE(analyst.ok());

    // Signed in as the analyst, the account-management calls must refuse.
    fixture.auth->signOut();
    ASSERT_TRUE(fixture.auth->signIn("analyst", "issued temporary password").ok());

    EXPECT_FALSE(fixture.auth->setRole(analyst.value().id, UserRole::Administrator).ok())
        << "an analyst who can promote themselves is not an analyst";
    EXPECT_FALSE(fixture.auth->setActive(analyst.value().id, false).ok());
    EXPECT_FALSE(fixture.auth->resetPassword(analyst.value().id, "a new long password").ok());
}

TEST(AuthTest, FirstRunClaimsAnIdentityThatWasRecordedButNeverGivenAPassword) {
    AuthFixture fixture("trace-auth-claim");

    // What an installation predating local accounts looks like, and what TRACE
    // still writes at startup so pre-sign-in audit rows name somebody: a users
    // row with no credential, which nobody can sign in as.
    UserRepository users(fixture.stack.database);
    UserAccount unclaimed;
    unclaimed.id = generateUuid();
    unclaimed.username = "operator";
    unclaimed.displayName = "Workstation operator";
    unclaimed.role = UserRole::Viewer;
    ASSERT_TRUE(users.upsert(unclaimed).ok());

    // It does not count as an account that can be signed into, so setup still runs.
    auto needed = fixture.auth->needsFirstRunSetup();
    ASSERT_TRUE(needed.ok());
    EXPECT_TRUE(needed.value());

    // And setup can take that name rather than refusing because it is "taken" by
    // a row nobody could ever use.
    auto claimed =
        fixture.auth->createFirstAdministrator("operator", "A. Analyst", kGoodPassword);
    ASSERT_TRUE(claimed.ok()) << claimed.error().message();
    EXPECT_EQ(claimed.value().role, UserRole::Administrator);

    // The same row was claimed, not a second one created — so audit history
    // already attributed to that identity stays attached to it.
    auto all = users.list();
    ASSERT_TRUE(all.ok());
    int matching = 0;
    for (const auto& account : all.value()) {
        if (account.username == "operator") ++matching;
    }
    EXPECT_EQ(matching, 1) << "first-run setup duplicated the identity instead of claiming it";

    auto stored = users.findStoredByUsername("operator");
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    EXPECT_EQ(stored.value()->account.id, unclaimed.id);
    EXPECT_TRUE(fixture.auth->signIn("operator", kGoodPassword).ok());
}

TEST(AuthTest, AnAccountThatAlreadyHasAPasswordIsNeverSilentlyOverwritten) {
    AuthFixture fixture("trace-auth-noclobber");
    ASSERT_TRUE(fixture.auth->createFirstAdministrator("admin", "Admin", kGoodPassword).ok());
    ASSERT_TRUE(fixture.auth->signIn("admin", kGoodPassword).ok());

    // Claiming is only ever for an identity with no credential. Reusing a name
    // that has one has to fail, or creating an account would be a way to take
    // over somebody else's.
    auto duplicate = fixture.auth->createAccount("admin", "Impostor", UserRole::Analyst,
                                                 "some other long password");
    ASSERT_FALSE(duplicate.ok());
    EXPECT_EQ(duplicate.error().code(), ErrorCode::InvalidArgument);

    // The original password still works and the role is unchanged.
    fixture.auth->signOut();
    auto signedIn = fixture.auth->signIn("admin", kGoodPassword);
    ASSERT_TRUE(signedIn.ok());
    EXPECT_EQ(signedIn.value().role, UserRole::Administrator);
}

TEST(AuthTest, CredentialsSurviveARestart) {
    testing::TemporaryDirectory dataRoot("trace-auth-restart");
    {
        auto stack = testing::TestStack::create(dataRoot.path());
        AuthService auth(stack.database, stack.audit);
        ASSERT_TRUE(auth.createFirstAdministrator("analyst", "A. Analyst", kGoodPassword).ok());
    }
    {
        auto stack = testing::TestStack::create(dataRoot.path());
        AuthService auth(stack.database, stack.audit);
        auto needed = auth.needsFirstRunSetup();
        ASSERT_TRUE(needed.ok());
        EXPECT_FALSE(needed.value()) << "the account did not survive reopening the database";
        EXPECT_TRUE(auth.signIn("analyst", kGoodPassword).ok());
    }
}

}  // namespace
}  // namespace trace
