#include "core/security/user_context.h"

#include <cstdlib>

#include "core/common/time_utils.h"
#include "core/common/uuid.h"

#if defined(_WIN32)
#include <windows.h>
#include <lmcons.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace trace {

const char* toString(UserRole role) {
    switch (role) {
        case UserRole::Viewer:        return "viewer";
        case UserRole::Analyst:       return "analyst";
        case UserRole::Supervisor:    return "supervisor";
        case UserRole::Administrator: return "administrator";
    }
    return "analyst";
}

UserRole userRoleFromString(const std::string& text, UserRole fallback) {
    if (text == "viewer") return UserRole::Viewer;
    if (text == "analyst") return UserRole::Analyst;
    if (text == "supervisor") return UserRole::Supervisor;
    if (text == "administrator") return UserRole::Administrator;
    return fallback;
}

const char* toString(Permission permission) {
    switch (permission) {
        case Permission::ViewCase:           return "view_case";
        case Permission::CreateCase:         return "create_case";
        case Permission::EditCase:           return "edit_case";
        case Permission::ImportEvidence:     return "import_evidence";
        case Permission::DeleteEvidence:     return "delete_evidence";
        case Permission::VerifyIntegrity:    return "verify_integrity";
        case Permission::CreateAnnotation:   return "create_annotation";
        case Permission::ExportDerivedAsset: return "export_derived_asset";
        case Permission::ManageSettings:     return "manage_settings";
        case Permission::ManageUsers:        return "manage_users";
    }
    return "unknown";
}

bool roleHasPermission(UserRole role, Permission permission) {
    switch (permission) {
        case Permission::ViewCase:
        case Permission::VerifyIntegrity:
            return true;  // every role may look and may check integrity
        case Permission::CreateCase:
        case Permission::EditCase:
        case Permission::ImportEvidence:
        case Permission::CreateAnnotation:
        case Permission::ExportDerivedAsset:
            return role >= UserRole::Analyst;
        case Permission::ManageSettings:
            return role >= UserRole::Supervisor;
        case Permission::DeleteEvidence:
        case Permission::ManageUsers:
            return role == UserRole::Administrator;
    }
    return false;
}

std::string detectOperatingSystemUser() {
#if defined(_WIN32)
    char buffer[UNLEN + 1];
    DWORD size = UNLEN + 1;
    if (GetUserNameA(buffer, &size) != 0) return std::string(buffer, size > 0 ? size - 1 : 0);
#else
    if (const char* env = std::getenv("USER"); env != nullptr && *env != '\0') return env;
    if (const char* env = std::getenv("LOGNAME"); env != nullptr && *env != '\0') return env;
    if (const struct passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_name != nullptr) {
        return pw->pw_name;
    }
#endif
    return "local-operator";
}

UserContext::UserContext() {
    account_.id = generateUuid();
    account_.username = detectOperatingSystemUser();
    account_.displayName = account_.username;
    // Phase 0 is a single-operator local deployment: the workstation user holds
    // full local authority. Multi-user deployments will load this from the
    // users table instead.
    account_.role = UserRole::Administrator;
    account_.createdAt = nowIso8601Utc();
    account_.lastSeenAt = account_.createdAt;
}

UserContext& UserContext::current() {
    static UserContext context;
    return context;
}

}  // namespace trace
