#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/security/keyring.h"
#include "core/security/workspace_keys.h"
#include "core/storage/storage_layout.h"

namespace trace {

class AuditService;

/// What TRACE found when it looked at a data directory, before opening
/// anything.
struct WorkspaceState {
    bool exists = false;              ///< there is a database or a keyring here
    bool encrypted = false;           ///< a keyring is present
    bool databaseEncrypted = false;   ///< the database file itself is keyed
    bool buildSupportsEncryption = false;
    std::vector<std::string> operators;

    /// True for a workspace whose conversion to encrypted storage did not
    /// finish: the keyring exists, so evidence is being written as containers,
    /// but the database is still readable on disk. Not a corrupt state — every
    /// reader handles both forms — but one an operator should be told about,
    /// because the protection they asked for is not fully in place.
    bool conversionIncomplete() const { return encrypted && exists && !databaseEncrypted; }
};

/// Progress of a bulk conversion. `false` from the callback cancels; a cancelled
/// conversion leaves a workspace that still opens, because each file is
/// converted completely or not at all.
struct ConversionProgress {
    std::int64_t filesDone = 0;
    std::int64_t filesTotal = 0;
    std::string currentEvidenceNumber;
};
using ConversionCallback = std::function<bool(const ConversionProgress&)>;

/// Setting up, unlocking and converting a TRACE workspace.
///
/// Everything here happens either before the case database is open or across
/// the whole of it, which is why it does not live on any of the per-entity
/// services.
class WorkspaceService {
public:
    /// Reads the data directory without opening or modifying anything.
    static WorkspaceState inspect(const std::filesystem::path& dataRoot);

    /// Creates the keyring for a new encrypted workspace. Does not convert
    /// anything: it is meant for a data directory with no cases in it yet.
    static Result<Keyring> createEncryptedWorkspace(const std::filesystem::path& dataRoot,
                                                    const std::string& username,
                                                    const std::string& password);

    /// Unlocks an existing encrypted workspace, filling `keys` on success.
    static Status unlock(const std::filesystem::path& dataRoot, const std::string& username,
                         const std::string& password, WorkspaceKeys& keys);

    /// Turns an existing unencrypted workspace into an encrypted one.
    ///
    /// ### Why this is resumable rather than atomic
    ///
    /// A single transaction across a case database and several hundred
    /// gigabytes of video does not exist. What makes an interruption safe here
    /// instead is that both forms of every file are valid at once: readers
    /// decide per file, by looking at it, so a directory holding some
    /// containers and some plain recordings is a working workspace rather than
    /// a half-migrated one. Conversion can therefore stop anywhere — a power
    /// cut, a cancelled dialog — and resume later by doing the files it has not
    /// reached.
    ///
    /// Each file individually is all-or-nothing: the container is written
    /// beside the original, decrypted, and its digest checked against the one
    /// recorded at ingestion before the original is replaced. A file whose
    /// digest does not match is left exactly as it was and the conversion
    /// stops, because at that point something is wrong that re-running will not
    /// fix.
    ///
    /// The database is re-keyed last, so an interrupted run leaves a workspace
    /// that still opens.
    static Status encryptExistingWorkspace(const std::filesystem::path& dataRoot,
                                           const std::string& username,
                                           const std::string& password,
                                           const ConversionCallback& progress = {});

    /// Converts any evidence still stored in the clear. Safe to call repeatedly;
    /// files that are already containers are skipped.
    static Status convertRemainingEvidence(Database& database, const StorageLayout& layout,
                                           const WorkspaceKeys& keys,
                                           const ConversionCallback& progress = {});

    /// Writes a keyed copy of `source` at `destination` using SQLCipher's own
    /// export, then swaps it in. The original is kept alongside as a `.plain`
    /// file rather than deleted — an operator who has just encrypted a
    /// workspace by mistake still has their case load, and deleting it here
    /// would be TRACE destroying evidence to tidy up after itself.
    static Status encryptDatabaseFile(const std::filesystem::path& path,
                                      const crypto::SecretKey& key);
};

}  // namespace trace
