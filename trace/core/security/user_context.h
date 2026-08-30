#pragma once

#include <string>
#include <vector>

namespace trace {

/// Roles TRACE will enforce once multi-user deployments arrive. Phase 0 runs as
/// a single local operator, but every service call already goes through the
/// permission gate below so that adding accounts later is a data change rather
/// than a rewrite.
enum class UserRole { Viewer = 0, Analyst, Supervisor, Administrator };

const char* toString(UserRole role);
UserRole userRoleFromString(const std::string& text, UserRole fallback = UserRole::Analyst);

enum class Permission {
    ViewCase,
    CreateCase,
    EditCase,
    ImportEvidence,
    DeleteEvidence,
    VerifyIntegrity,
    CreateAnnotation,
    ExportDerivedAsset,
    ManageSettings,
    ManageUsers,
};

const char* toString(Permission permission);
bool roleHasPermission(UserRole role, Permission permission);

/// A local account record. Phase 0 persists exactly one — the operator running
/// the workstation — and never stores a credential; `passwordHash` exists so the
/// migration that introduces real authentication does not have to rewrite rows.
struct UserAccount {
    std::string id;
    std::string username;
    std::string displayName;
    UserRole role = UserRole::Analyst;
    bool active = true;
    std::string createdAt;
    std::string lastSeenAt;
};

/// Identity of the operator whose actions are recorded in the audit trail.
///
/// Identity and authority are separate here. Naming somebody in an audit row is
/// not the same as having established that they are who the row says, and the
/// two used to be conflated: the context started life holding the workstation
/// user with Administrator authority, from a time when TRACE trusted whoever
/// opened it. Once accounts exist that default is a standing grant to anyone who
/// reaches a code path before sign-in.
///
/// So permissions are withheld until AuthService has actually verified a
/// credential. setAccount() names an operator and grants nothing; only
/// setAuthenticatedAccount() confers authority, and only AuthService calls it.
class UserContext {
public:
    static UserContext& current();

    const UserAccount& account() const { return account_; }

    /// Names the operator without granting anything — used for the audit rows
    /// written before anyone has signed in, such as the schema migration.
    void setAccount(UserAccount account) {
        account_ = std::move(account);
        authenticated_ = false;
    }

    /// Names the operator *and* confers their role's authority. Called only
    /// after a credential has been verified.
    void setAuthenticatedAccount(UserAccount account) {
        account_ = std::move(account);
        authenticated_ = true;
    }

    void clear() {
        account_ = UserAccount{};
        authenticated_ = false;
    }

    bool authenticated() const { return authenticated_; }

    const std::string& actorName() const { return account_.username; }

    /// False for everything until somebody has signed in. A default-constructed
    /// UserAccount is nominally an Analyst, so without this check "nobody" would
    /// hold an analyst's authority the moment a session ended.
    bool can(Permission permission) const {
        return authenticated_ && roleHasPermission(account_.role, permission);
    }

private:
    UserContext();
    UserAccount account_;
    bool authenticated_ = false;
};

/// Best-effort operating-system user name, used to seed the local account so
/// audit records name a real operator instead of a placeholder.
std::string detectOperatingSystemUser();

}  // namespace trace
