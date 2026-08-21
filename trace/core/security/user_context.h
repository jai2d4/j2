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
class UserContext {
public:
    static UserContext& current();

    const UserAccount& account() const { return account_; }
    void setAccount(UserAccount account) { account_ = std::move(account); }

    const std::string& actorName() const { return account_.username; }
    bool can(Permission permission) const { return roleHasPermission(account_.role, permission); }

private:
    UserContext();
    UserAccount account_;
};

/// Best-effort operating-system user name, used to seed the local account so
/// audit records name a real operator instead of a placeholder.
std::string detectOperatingSystemUser();

}  // namespace trace
