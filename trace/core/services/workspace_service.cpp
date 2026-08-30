#include "core/services/workspace_service.h"

#include <fstream>

#include "core/common/logging.h"
#include "core/models/evidence.h"
#include "core/security/file_hasher.h"
#include "core/storage/file_operations.h"

namespace trace {
namespace {

constexpr const char* kComponent = "workspace";

}  // namespace

WorkspaceState WorkspaceService::inspect(const std::filesystem::path& dataRoot) {
    WorkspaceState state;
    state.buildSupportsEncryption = crypto::available();

    const StorageLayout layout(dataRoot);
    std::error_code ec;
    const auto databasePath = layout.databasePath();
    const bool databaseExists = std::filesystem::exists(databasePath, ec);

    // A keyring is what makes a workspace encrypted, not the state of the
    // database file. Deciding it the other way round would leave an interrupted
    // conversion looking unencrypted, and the next ingestion would write
    // evidence in the clear into a workspace that already holds containers.
    state.encrypted = Keyring::exists(dataRoot);
    state.exists = databaseExists || state.encrypted;
    state.databaseEncrypted = databaseExists && Database::fileIsEncrypted(databasePath);

    if (state.encrypted) {
        if (auto keyring = Keyring::load(dataRoot); keyring) {
            state.operators = keyring.take().operators();
        }
    }
    return state;
}

Result<Keyring> WorkspaceService::createEncryptedWorkspace(const std::filesystem::path& dataRoot,
                                                           const std::string& username,
                                                           const std::string& password) {
    const StorageLayout layout(dataRoot);
    if (auto status = layout.ensureDataRoot(); !status) {
        return Result<Keyring>(status.error());
    }
    return Keyring::create(dataRoot, username, password);
}

Status WorkspaceService::unlock(const std::filesystem::path& dataRoot, const std::string& username,
                                const std::string& password, WorkspaceKeys& keys) {
    auto keyring = Keyring::load(dataRoot);
    if (!keyring) return Status(keyring.error());

    auto masterKey = keyring.take().unlock(username, password);
    if (!masterKey) return Status(masterKey.error());

    keys.unlock(masterKey.take());
    logInfo(kComponent, "Workspace unlocked", JsonValue::object().set("username", username));
    return Status::success();
}

Status WorkspaceService::encryptDatabaseFile(const std::filesystem::path& path,
                                             const crypto::SecretKey& key) {
    if (!crypto::available()) {
        return Status::failure(ErrorCode::Unsupported,
                               "This build of TRACE cannot encrypt a case database");
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // Nothing to convert: the next open creates a keyed database directly.
        return Status::success();
    }
    if (Database::fileIsEncrypted(path)) return Status::success();

    const auto encryptedPath = path.string() + ".encrypting";
    const auto keptPlainPath = path.string() + ".plain";
    std::filesystem::remove(encryptedPath, ec);

    {
        auto opened = Database::open(path);
        if (!opened) return Status(opened.error());
        auto database = opened.take();

        // sqlcipher_export copies every page of the attached database through
        // the codec. Doing it this way rather than by re-running migrations
        // means the audit trail, the digests and every row identifier survive
        // exactly as they are — an encrypted workspace has the same history as
        // the one it replaced, which is the whole point in an evidence tool.
        const std::string attach = "ATTACH DATABASE '" + encryptedPath +
                                   "' AS encrypted KEY \"x'" + key.toHexForSqlCipher() + "'\";";
        if (auto status = database->execute(attach); !status) return status;
        if (auto status = database->execute("SELECT sqlcipher_export('encrypted');"); !status) {
            database->execute("DETACH DATABASE encrypted;");
            std::filesystem::remove(encryptedPath, ec);
            return status;
        }
        // The schema version travels with the data; without it the new database
        // looks unmigrated and the next start would try to apply every
        // migration again.
        auto version = database->queryInt64("PRAGMA user_version;");
        if (version) {
            database->execute("PRAGMA encrypted.user_version = " +
                              std::to_string(version.take()) + ";");
        }
        if (auto status = database->execute("DETACH DATABASE encrypted;"); !status) return status;
    }

    // Prove the copy opens with the key before anything moves. A rename that
    // puts an unreadable database in place is not recoverable by re-running.
    {
        auto check = Database::openEncrypted(encryptedPath, key);
        if (!check) {
            std::filesystem::remove(encryptedPath, ec);
            return Status(check.error());
        }
    }

    // Keep the plaintext database rather than deleting it. An operator who has
    // just encrypted a workspace by accident still has their case load, and
    // TRACE deleting the only copy of a case index to tidy up after itself is
    // not a trade it gets to make on their behalf.
    std::filesystem::rename(path, keptPlainPath, ec);
    if (ec) {
        std::filesystem::remove(encryptedPath, ec);
        return Status::failure(ErrorCode::IoError, "Unable to set the plaintext database aside",
                               ec.message());
    }
    std::filesystem::rename(encryptedPath, path, ec);
    if (ec) {
        // Put it back; the workspace must still open.
        std::error_code restoreEc;
        std::filesystem::rename(keptPlainPath, path, restoreEc);
        return Status::failure(ErrorCode::IoError, "Unable to install the encrypted database",
                               ec.message());
    }

    logInfo(kComponent, "Case database encrypted",
            JsonValue::object().set("previous_kept_at", keptPlainPath));
    return Status::success();
}

Status WorkspaceService::convertRemainingEvidence(Database& database, const StorageLayout& layout,
                                                  const WorkspaceKeys& keys,
                                                  const ConversionCallback& progress) {
    if (!keys.unlocked()) {
        return Status::failure(ErrorCode::PermissionDenied,
                               "The workspace must be unlocked before its evidence can be encrypted");
    }

    auto statement = database.prepare(
        "SELECT id, case_id, evidence_number, storage_relpath, sha256 FROM evidence "
        "ORDER BY evidence_number;");
    if (!statement) return Status(statement.error());

    struct Item {
        std::string id;
        std::string caseId;
        std::string evidenceNumber;
        std::string relPath;
        std::string sha256;
    };
    std::vector<Item> items;
    {
        auto query = statement.take();
        while (true) {
            auto stepped = query.step();
            if (!stepped) return Status(stepped.error());
            if (!stepped.take()) break;
            items.push_back(Item{query.columnText(0), query.columnText(1), query.columnText(2),
                                 query.columnText(3), query.columnText(4)});
        }
    }

    ConversionProgress state;
    state.filesTotal = static_cast<std::int64_t>(items.size());

    for (const Item& item : items) {
        state.currentEvidenceNumber = item.evidenceNumber;
        if (progress && !progress(state)) {
            return Status::failure(ErrorCode::Cancelled, "Encryption cancelled by operator");
        }

        const auto path = layout.resolve(item.relPath);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            // A missing original is a real problem, but it is the integrity
            // check's problem, not this one's. Reporting it here would stop a
            // conversion that has nothing to do with it.
            logError(kComponent, "Skipping evidence that is missing from storage",
                     JsonValue::object().set("evidence", item.evidenceNumber));
            ++state.filesDone;
            continue;
        }
        if (crypto::looksEncrypted(path)) {
            ++state.filesDone;
            continue;
        }

        auto caseKey = keys.caseKey(item.caseId);
        if (!caseKey) return Status(caseKey.error());
        const auto key = caseKey.take();

        const auto containerPath = path.string() + ".encrypting";
        std::filesystem::remove(containerPath, ec);

        // Written beside the original and verified before anything is replaced.
        auto copied = copyIntoManagedStorage(path, containerPath, {}, &key);
        if (!copied) {
            std::filesystem::remove(containerPath, ec);
            return Status(copied.error());
        }
        const CopyOutcome outcome = copied.take();

        // The digest recorded at ingestion is the one that has to survive. If
        // the container does not decrypt back to it, the file is left exactly
        // as it was and the conversion stops: re-running will not fix whatever
        // caused this, and continuing would encrypt more evidence into a
        // process that has already produced one wrong answer.
        if (outcome.destinationSha256 != item.sha256) {
            std::filesystem::remove(containerPath, ec);
            logError(kComponent, "Refusing to replace evidence whose encrypted copy did not match",
                     JsonValue::object()
                         .set("evidence", item.evidenceNumber)
                         .set("stored_sha256", item.sha256)
                         .set("copy_sha256", outcome.destinationSha256));
            return Status::failure(
                ErrorCode::IntegrityFailure,
                "The encrypted copy of " + item.evidenceNumber + " did not match its stored digest",
                "The original is untouched. Verify this item before encrypting the workspace.");
        }

        // The managed original is read-only, which is what stops accidental
        // edits; replacing it deliberately means clearing that first.
        std::filesystem::permissions(path, std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::rename(containerPath, path, ec);
        if (ec) {
            std::filesystem::remove(containerPath, ec);
            return Status::failure(ErrorCode::IoError,
                                   "Unable to replace " + item.evidenceNumber + " with its "
                                   "encrypted copy", ec.message());
        }
        setReadOnly(path);
        ++state.filesDone;
    }

    if (progress) progress(state);
    logInfo(kComponent, "Evidence encryption pass complete",
            JsonValue::object().set("files", state.filesDone));
    return Status::success();
}

Status WorkspaceService::encryptExistingWorkspace(const std::filesystem::path& dataRoot,
                                                  const std::string& username,
                                                  const std::string& password,
                                                  const ConversionCallback& progress) {
    const WorkspaceState state = inspect(dataRoot);
    if (!state.buildSupportsEncryption) {
        return Status::failure(ErrorCode::Unsupported, "This build of TRACE cannot encrypt");
    }

    WorkspaceKeys keys;
    if (state.encrypted) {
        // Resuming an interrupted conversion, which is an ordinary thing to be
        // doing and not a reason to refuse.
        if (auto status = unlock(dataRoot, username, password, keys); !status) return status;
    } else {
        auto keyring = createEncryptedWorkspace(dataRoot, username, password);
        if (!keyring) return Status(keyring.error());
        auto masterKey = keyring.take().unlock(username, password);
        if (!masterKey) return Status(masterKey.error());
        keys.unlock(masterKey.take());
    }

    const StorageLayout layout(dataRoot);
    const auto databasePath = layout.databasePath();

    // Evidence first, database last. An interruption then leaves a workspace
    // that still opens: the keyring says it is encrypted, the readers handle
    // whichever form each file is in, and running this again finishes the job.
    {
        auto opened = Database::fileIsEncrypted(databasePath)
                          ? Database::openEncrypted(databasePath, keys.masterKey().take())
                          : Database::open(databasePath);
        if (!opened) return Status(opened.error());
        auto database = opened.take();
        if (auto status = convertRemainingEvidence(*database, layout, keys, progress); !status) {
            return status;
        }
    }

    auto masterKey = keys.masterKey();
    if (!masterKey) return Status(masterKey.error());
    return encryptDatabaseFile(databasePath, masterKey.take());
}

}  // namespace trace
