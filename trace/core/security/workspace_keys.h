#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/common/result.h"
#include "core/security/crypto.h"

namespace trace {

/// Holds the keys for an open workspace, and hands out the per-case key that
/// evidence containers are written and read with.
///
/// ## Per-case keys are derived, not stored
///
/// A case key is `HKDF(masterKey, salt = case id, info = "trace-case-v1")`. It
/// exists only in memory, for as long as it is being used, and there is no table
/// of wrapped case keys to fall out of step with the list of cases — a row that
/// went missing would make a case unreadable while every other part of TRACE
/// still believed it was fine.
///
/// The cost of deriving rather than storing is worth naming, because it is a
/// real workflow and not a hypothetical: handing one case to another agency
/// would mean handing over its key, and a derived key cannot be given away
/// without giving away the master. Disclosure today therefore means exporting an
/// exhibit bundle, which is unencrypted by design and verifiable without TRACE.
/// If per-case key transfer is ever wanted, wrapped case keys become a migration
/// — the container format already takes a key per file and does not care where
/// it came from.
///
/// ## Locked is the default
///
/// A newly constructed instance holds nothing. Everything that needs a key gets
/// `NotFound` until `unlock()` has been called with a master key that a keyring
/// actually produced, which means no code path can quietly proceed unencrypted
/// because a key was missing.
class WorkspaceKeys {
public:
    WorkspaceKeys() = default;

    /// Records that this workspace stores its data encrypted, without yet
    /// holding the key. This is the state between "TRACE found a keyring" and
    /// "somebody signed in", and it is why an empty key handle can be told apart
    /// from a locked one.
    void markEncrypted();

    /// Adopts the master key for this session. Called once, after the keyring
    /// has verified a password. Implies markEncrypted().
    void unlock(crypto::SecretKey masterKey);

    /// Drops the key. Anything already decrypted stays decrypted — this is not
    /// a way to recall data that has been read — but nothing further opens.
    void lock();

    bool unlocked() const;
    /// True for a workspace whose data is stored encrypted, whether or not it is
    /// currently unlocked.
    bool encrypted() const;

    /// The workspace master key, for the case database.
    Result<crypto::SecretKey> masterKey() const;

    /// The key for one case's evidence containers.
    Result<crypto::SecretKey> caseKey(const std::string& caseId) const;

private:
    mutable std::mutex mutex_;
    std::optional<crypto::SecretKey> masterKey_;
    bool encrypted_ = false;
};

/// Borrowed access to a case key, or to nothing.
///
/// Ingestion, decoding and verification all take `const crypto::SecretKey*`,
/// where null means "this file is not encrypted". This holds the derived key
/// alive for the length of an operation and produces that pointer, so callers do
/// not each invent their own optional-key plumbing.
class CaseKeyHandle {
public:
    CaseKeyHandle() = default;
    explicit CaseKeyHandle(crypto::SecretKey key) : key_(std::move(key)) {}

    /// Null when the workspace is not encrypted, which is what every code path
    /// downstream reads as "plain file".
    const crypto::SecretKey* get() const { return key_ ? &*key_ : nullptr; }
    bool present() const { return key_.has_value(); }

private:
    std::optional<crypto::SecretKey> key_;
};

/// The case key when the workspace is unlocked, an empty handle when it is not
/// an encrypted workspace at all.
///
/// A locked-but-encrypted workspace is the one case that must not silently
/// produce an empty handle, because that would ingest evidence in the clear into
/// a workspace whose operator believes it is encrypted. That case is an error.
Result<CaseKeyHandle> caseKeyFor(const WorkspaceKeys& keys, const std::string& caseId);

}  // namespace trace
