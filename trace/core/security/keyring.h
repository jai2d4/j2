#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/security/crypto.h"

namespace trace {

/// The workspace keyring: how a password becomes the key to an encrypted case
/// database, and how more than one operator can hold that key without sharing a
/// password.
///
/// ## The hierarchy
///
///     password ──PBKDF2─▶ wrapping key ──AES-GCM─▶ [wrapped master key]
///                                                        │
///     master key ────────────────────────────────────────┘
///        ├── SQLCipher key for trace.db
///        └── wraps a per-case key for evidence containers
///
/// One master key exists per workspace, and the keyring stores it once per
/// operator, each copy wrapped under that operator's own password. Two
/// consequences follow, and both are the reason for the indirection:
///
/// - Changing a password rewraps one 32-byte value. It does not re-encrypt a
///   single byte of evidence, so it takes the same time on a workstation
///   holding one case as on one holding a thousand.
/// - Adding a second investigator does not mean sharing a password, and
///   removing one does not mean re-keying the workspace.
///
/// ## Why this file and not the database
///
/// The master key opens the database, so the wrapped copies cannot live inside
/// it. The keyring is therefore a separate file, in the clear as far as its
/// structure goes: usernames, salts and work factors are all readable. None of
/// that is secret. What it does not contain, in any form, is the master key or
/// any password — only values that need one to unwrap.
///
/// ## What an attacker with this file gets
///
/// An offline guessing target, at 600,000 PBKDF2 iterations per guess per
/// account. That is the honest description: a weak password is still a weak
/// password, and the keyring makes each attempt expensive rather than
/// impossible. This is stated here so nobody reads "encrypted at rest" as a
/// reason to accept a short one.
class Keyring {
public:
    /// One operator's wrapped copy of the master key.
    struct Entry {
        std::string username;
        std::uint32_t iterations = 0;
        std::vector<std::uint8_t> salt;
        std::vector<std::uint8_t> wrappedKey;
    };

    /// Default filename inside the data root.
    static constexpr const char* kFilename = "keyring.tkr";

    static std::filesystem::path pathFor(const std::filesystem::path& dataRoot);
    /// Whether this workspace has been set up for encryption at all. A missing
    /// keyring is not an error: it is what an unencrypted workspace looks like.
    static bool exists(const std::filesystem::path& dataRoot);

    /// Creates a keyring with a fresh master key and one operator. Refuses to
    /// overwrite an existing one — that would strand every encrypted case in the
    /// workspace behind a key that no longer exists anywhere.
    static Result<Keyring> create(const std::filesystem::path& dataRoot,
                                  const std::string& username, const std::string& password);

    static Result<Keyring> load(const std::filesystem::path& dataRoot);

    /// Recovers the master key. A wrong password, an unknown user and a damaged
    /// entry all fail the same way, because distinguishing them would say which
    /// usernames exist.
    Result<crypto::SecretKey> unlock(const std::string& username,
                                     const std::string& password) const;

    /// Adds an operator. Requires the master key, so only somebody who has
    /// already unlocked the workspace can extend access to it.
    Status addOperator(const crypto::SecretKey& masterKey, const std::string& username,
                       const std::string& password);

    /// Rewraps one operator's copy under a new password. The master key, and
    /// therefore every encrypted byte in the workspace, is untouched.
    Status changePassword(const std::string& username, const std::string& currentPassword,
                          const std::string& newPassword);

    /// Removes an operator's access. Refuses to remove the last one: a keyring
    /// with no entries is a workspace nobody can ever open again, and no
    /// confirmation dialog makes that recoverable.
    Status removeOperator(const std::string& username);

    bool hasOperator(const std::string& username) const;
    std::vector<std::string> operators() const;
    std::size_t operatorCount() const { return entries_.size(); }

    /// Identifies this workspace. Bound into every wrap, so an entry lifted from
    /// one keyring into another fails to unwrap rather than granting access.
    const std::vector<std::uint8_t>& workspaceId() const { return workspaceId_; }
    std::string workspaceIdHex() const;

    const std::filesystem::path& path() const { return path_; }

private:
    Status save() const;
    static std::string associatedDataFor(const std::vector<std::uint8_t>& workspaceId,
                                         const std::string& username);

    std::filesystem::path path_;
    std::vector<std::uint8_t> workspaceId_;
    std::vector<Entry> entries_;
};

}  // namespace trace
