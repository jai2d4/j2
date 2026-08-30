#include "core/security/keyring.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "core/common/logging.h"
#include "core/security/password.h"

namespace trace {
namespace {

constexpr const char* kComponent = "keyring";
constexpr char kMagic[8] = {'T', 'R', 'A', 'C', 'E', 'K', 'R', '1'};
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kKdfPbkdf2Sha256 = 1;
constexpr std::size_t kWorkspaceIdBytes = 16;
/// Sanity bounds. A keyring is a small file; anything claiming otherwise is
/// corrupt or hostile, and the reader should say so rather than allocate.
constexpr std::uint16_t kMaxUsernameBytes = 256;
constexpr std::uint16_t kMaxSaltBytes = 256;
constexpr std::uint16_t kMaxWrappedBytes = 512;
constexpr std::uint16_t kMaxEntries = 1024;

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
}

void appendBytes(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// A cursor that refuses to read past the end.
///
/// Every length in this format comes from the file being parsed, which is the
/// classic way a parser is turned into a vulnerability. Nothing here reads a
/// length and then trusts it; `ok_` latches false on the first overrun and every
/// subsequent read is a no-op, so a truncated or hostile file produces an empty
/// result rather than a read out of bounds.
class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t bytes) : data_(data), bytes_(bytes) {}

    bool ok() const { return ok_; }
    std::size_t remaining() const { return ok_ ? bytes_ - at_ : 0; }
    bool exhausted() const { return ok_ && at_ == bytes_; }

    bool take(std::size_t count, std::vector<std::uint8_t>* into) {
        if (!ok_ || count > bytes_ - at_) return fail();
        into->assign(data_ + at_, data_ + at_ + count);
        at_ += count;
        return true;
    }

    bool takeString(std::size_t count, std::string* into) {
        if (!ok_ || count > bytes_ - at_) return fail();
        into->assign(reinterpret_cast<const char*>(data_ + at_), count);
        at_ += count;
        return true;
    }

    bool skipEqual(const char* expected, std::size_t count) {
        if (!ok_ || count > bytes_ - at_) return fail();
        if (std::memcmp(data_ + at_, expected, count) != 0) return fail();
        at_ += count;
        return true;
    }

    bool u16(std::uint16_t* into) {
        if (!ok_ || bytes_ - at_ < 2) return fail();
        *into = static_cast<std::uint16_t>(data_[at_]) |
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[at_ + 1]) << 8);
        at_ += 2;
        return true;
    }

    bool u32(std::uint32_t* into) {
        if (!ok_ || bytes_ - at_ < 4) return fail();
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data_[at_ + i]) << (8 * i);
        *into = value;
        at_ += 4;
        return true;
    }

private:
    bool fail() {
        ok_ = false;
        return false;
    }

    const std::uint8_t* data_;
    std::size_t bytes_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

Status malformed(const std::string& detail) {
    return Status::failure(ErrorCode::IntegrityFailure, "The workspace keyring is not readable",
                           detail);
}

/// One answer for every failed unlock. A wrong password, a username that does
/// not exist and a corrupted entry are indistinguishable to the caller on
/// purpose: anything else turns the unlock dialog into a way to find out which
/// operators a workstation has.
Error wrongCredential() {
    return Error(ErrorCode::PermissionDenied, "That username and password did not unlock this workspace",
                 "Check both. If this workspace was set up on another machine, the operator who "
                 "created it has to add you to it.");
}

}  // namespace

std::filesystem::path Keyring::pathFor(const std::filesystem::path& dataRoot) {
    return dataRoot / kFilename;
}

bool Keyring::exists(const std::filesystem::path& dataRoot) {
    std::error_code ec;
    return std::filesystem::exists(pathFor(dataRoot), ec);
}

std::string Keyring::associatedDataFor(const std::vector<std::uint8_t>& workspaceId,
                                       const std::string& username) {
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(workspaceId.size() * 2);
    for (std::uint8_t byte : workspaceId) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0F]);
    }
    // Binding both the workspace and the username means a wrapped key cannot be
    // copied into another keyring, and cannot be relabelled to another operator
    // inside this one — either edit changes the associated data and the unwrap
    // fails.
    return "trace-keyring-v1:" + hex + ":" + username;
}

std::string Keyring::workspaceIdHex() const {
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(workspaceId_.size() * 2);
    for (std::uint8_t byte : workspaceId_) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0F]);
    }
    return hex;
}

Result<Keyring> Keyring::create(const std::filesystem::path& dataRoot, const std::string& username,
                                const std::string& password) {
    using ResultType = Result<Keyring>;
    if (!crypto::available()) {
        return ResultType::failure(
            ErrorCode::Unsupported, "This build of TRACE cannot encrypt",
            "It was built without SQLCipher or OpenSSL. An unencrypted workspace still works.");
    }
    if (username.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "An operator needs a username");
    }
    if (auto strength = password::checkStrength(password); !strength) {
        return ResultType(strength.error());
    }

    const auto path = pathFor(dataRoot);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        // Overwriting this file destroys the only copies of the master key, and
        // with them every encrypted case in the workspace. There is no version
        // of that which is recoverable, so it is refused rather than confirmed.
        return ResultType::failure(
            ErrorCode::AlreadyExists, "This workspace already has a keyring",
            "Overwriting it would make every encrypted case in it permanently unreadable.");
    }

    auto workspaceId = password::randomBytes(kWorkspaceIdBytes);
    if (!workspaceId) return ResultType(workspaceId.error());
    auto masterKey = crypto::SecretKey::random();
    if (!masterKey) return ResultType(masterKey.error());

    Keyring keyring;
    keyring.path_ = path;
    keyring.workspaceId_ = workspaceId.take();

    if (auto status = keyring.addOperator(masterKey.take(), username, password); !status) {
        return ResultType(status.error());
    }
    logInfo(kComponent, "Workspace keyring created",
            JsonValue::object().set("workspace", keyring.workspaceIdHex()));
    return ResultType::success(std::move(keyring));
}

Result<Keyring> Keyring::load(const std::filesystem::path& dataRoot) {
    using ResultType = Result<Keyring>;
    const auto path = pathFor(dataRoot);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return ResultType::failure(ErrorCode::NotFound,
                                   "This workspace has no keyring: " + path.string());
    }
    const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());

    Cursor cursor(raw.data(), raw.size());
    Keyring keyring;
    keyring.path_ = path;

    if (!cursor.skipEqual(kMagic, sizeof(kMagic))) {
        return ResultType(malformed("It does not begin with the keyring marker.").error());
    }
    std::uint16_t version = 0;
    std::uint16_t entryCount = 0;
    std::uint32_t reserved = 0;
    if (!cursor.u16(&version) || !cursor.u16(&entryCount) || !cursor.u32(&reserved)) {
        return ResultType(malformed("The header is incomplete.").error());
    }
    if (version != kVersion) {
        return ResultType::failure(ErrorCode::Unsupported, "Unsupported keyring version",
                                   "This file declares version " + std::to_string(version) +
                                       "; this build understands " + std::to_string(kVersion) + ".");
    }
    if (entryCount > kMaxEntries) {
        return ResultType(malformed("It declares an implausible number of operators.").error());
    }
    if (!cursor.take(kWorkspaceIdBytes, &keyring.workspaceId_)) {
        return ResultType(malformed("The workspace identifier is missing.").error());
    }

    for (std::uint16_t i = 0; i < entryCount; ++i) {
        Entry entry;
        std::uint16_t usernameBytes = 0;
        std::uint16_t kdf = 0;
        std::uint16_t saltBytes = 0;
        std::uint16_t wrappedBytes = 0;

        if (!cursor.u16(&usernameBytes) || usernameBytes > kMaxUsernameBytes ||
            !cursor.takeString(usernameBytes, &entry.username) || !cursor.u16(&kdf) ||
            !cursor.u32(&entry.iterations) || !cursor.u16(&saltBytes) ||
            saltBytes > kMaxSaltBytes || !cursor.take(saltBytes, &entry.salt) ||
            !cursor.u16(&wrappedBytes) || wrappedBytes > kMaxWrappedBytes ||
            !cursor.take(wrappedBytes, &entry.wrappedKey)) {
            return ResultType(malformed("Entry " + std::to_string(i) + " is truncated.").error());
        }
        if (kdf != kKdfPbkdf2Sha256) {
            return ResultType::failure(ErrorCode::Unsupported,
                                       "The keyring uses a key derivation this build does not know",
                                       "Entry for '" + entry.username + "'.");
        }
        if (entry.iterations == 0 || entry.salt.empty() || entry.wrappedKey.empty()) {
            return ResultType(malformed("Entry " + std::to_string(i) + " is incomplete.").error());
        }
        // Two entries for one username would make "which one is authoritative"
        // a question, and an attacker who can append to this file would get to
        // answer it.
        const bool duplicate = std::any_of(
            keyring.entries_.begin(), keyring.entries_.end(),
            [&entry](const Entry& existing) { return existing.username == entry.username; });
        if (duplicate) {
            return ResultType(malformed("It lists '" + entry.username + "' twice.").error());
        }
        keyring.entries_.push_back(std::move(entry));
    }

    if (!cursor.exhausted()) {
        // Trailing bytes mean the file is not what it says it is. Ignoring them
        // is how an appended second keyring goes unnoticed.
        return ResultType(malformed("It has trailing data after the last operator.").error());
    }
    if (keyring.entries_.empty()) {
        return ResultType(malformed("It lists no operators, so nothing could ever open it.").error());
    }
    return ResultType::success(std::move(keyring));
}

Status Keyring::save() const {
    if (entries_.empty()) {
        return Status::failure(ErrorCode::Internal,
                               "Refusing to write a keyring with no operators");
    }
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
    appendU16(out, kVersion);
    appendU16(out, static_cast<std::uint16_t>(entries_.size()));
    appendU32(out, 0);
    appendBytes(out, workspaceId_);

    for (const Entry& entry : entries_) {
        appendU16(out, static_cast<std::uint16_t>(entry.username.size()));
        out.insert(out.end(), entry.username.begin(), entry.username.end());
        appendU16(out, kKdfPbkdf2Sha256);
        appendU32(out, entry.iterations);
        appendU16(out, static_cast<std::uint16_t>(entry.salt.size()));
        appendBytes(out, entry.salt);
        appendU16(out, static_cast<std::uint16_t>(entry.wrappedKey.size()));
        appendBytes(out, entry.wrappedKey);
    }

    // Write beside the target and rename over it. A machine that loses power
    // half way through a password change must still have a keyring afterwards:
    // a partially written one is a workspace nobody can open.
    std::error_code ec;
    const auto parent = path_.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    const auto temporary = path_.string() + ".new";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Status::failure(ErrorCode::IoError,
                                   "Unable to write the workspace keyring: " + temporary);
        }
        file.write(reinterpret_cast<const char*>(out.data()),
                   static_cast<std::streamsize>(out.size()));
        file.flush();
        if (!file) {
            return Status::failure(ErrorCode::IoError, "Unable to write the workspace keyring");
        }
    }
    std::filesystem::rename(temporary, path_, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return Status::failure(ErrorCode::IoError, "Unable to replace the workspace keyring",
                               ec.message());
    }
    return Status::success();
}

Result<crypto::SecretKey> Keyring::unlock(const std::string& username,
                                          const std::string& plaintext) const {
    using ResultType = Result<crypto::SecretKey>;
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&username](const Entry& entry) {
                                        return entry.username == username;
                                    });
    if (found == entries_.end()) {
        // Deliberately no early return before the work: a username that does not
        // exist should not be measurably faster to reject than a wrong password.
        // The derivation below is thrown away; its cost is the point.
        if (!entries_.empty()) {
            (void)crypto::SecretKey::fromPassword(plaintext, entries_.front().salt,
                                                  entries_.front().iterations);
        }
        return ResultType(wrongCredential());
    }

    auto wrappingKey = crypto::SecretKey::fromPassword(plaintext, found->salt, found->iterations);
    if (!wrappingKey) return ResultType(wrappingKey.error());

    auto unwrapped = crypto::unseal(wrappingKey.take(), found->wrappedKey,
                                    associatedDataFor(workspaceId_, username));
    if (!unwrapped) return ResultType(wrongCredential());

    return crypto::SecretKey::fromBytes(unwrapped.take());
}

Status Keyring::addOperator(const crypto::SecretKey& masterKey, const std::string& username,
                            const std::string& plaintext) {
    if (username.empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "An operator needs a username");
    }
    if (hasOperator(username)) {
        return Status::failure(ErrorCode::AlreadyExists,
                               "'" + username + "' can already open this workspace");
    }
    if (auto strength = password::checkStrength(plaintext); !strength) return strength;

    auto salt = password::randomBytes(password::kSaltBytes);
    if (!salt) return Status(salt.error());
    Entry entry;
    entry.username = username;
    entry.iterations = password::kDefaultIterations;
    entry.salt = salt.take();

    auto wrappingKey = crypto::SecretKey::fromPassword(plaintext, entry.salt, entry.iterations);
    if (!wrappingKey) return Status(wrappingKey.error());

    const std::vector<std::uint8_t> keyBytes(masterKey.data(), masterKey.data() + masterKey.size());
    auto wrapped = crypto::seal(wrappingKey.take(), keyBytes,
                                associatedDataFor(workspaceId_, username));
    if (!wrapped) return Status(wrapped.error());
    entry.wrappedKey = wrapped.take();

    entries_.push_back(std::move(entry));
    if (auto status = save(); !status) {
        entries_.pop_back();
        return status;
    }
    logInfo(kComponent, "Operator added to the workspace keyring",
            JsonValue::object().set("username", username));
    return Status::success();
}

Status Keyring::changePassword(const std::string& username, const std::string& currentPassword,
                               const std::string& newPassword) {
    auto masterKey = unlock(username, currentPassword);
    if (!masterKey) return Status(masterKey.error());
    if (auto strength = password::checkStrength(newPassword); !strength) return strength;

    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&username](const Entry& entry) {
                                        return entry.username == username;
                                    });
    if (found == entries_.end()) return Status(wrongCredential());

    auto salt = password::randomBytes(password::kSaltBytes);
    if (!salt) return Status(salt.error());
    const Entry previous = *found;

    found->salt = salt.take();
    found->iterations = password::kDefaultIterations;
    auto wrappingKey = crypto::SecretKey::fromPassword(newPassword, found->salt, found->iterations);
    if (!wrappingKey) {
        *found = previous;
        return Status(wrappingKey.error());
    }
    const auto key = masterKey.take();
    const std::vector<std::uint8_t> keyBytes(key.data(), key.data() + key.size());
    auto wrapped = crypto::seal(wrappingKey.take(), keyBytes,
                                associatedDataFor(workspaceId_, username));
    if (!wrapped) {
        *found = previous;
        return Status(wrapped.error());
    }
    found->wrappedKey = wrapped.take();

    if (auto status = save(); !status) {
        // Put the old entry back in memory too: a keyring that failed to write
        // must not leave this object describing a file that does not exist.
        *found = previous;
        return status;
    }
    logInfo(kComponent, "Keyring entry rewrapped under a new password",
            JsonValue::object().set("username", username));
    return Status::success();
}

Status Keyring::removeOperator(const std::string& username) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&username](const Entry& entry) {
                                        return entry.username == username;
                                    });
    if (found == entries_.end()) {
        return Status::failure(ErrorCode::NotFound,
                               "'" + username + "' cannot open this workspace anyway");
    }
    if (entries_.size() == 1) {
        return Status::failure(
            ErrorCode::InvalidArgument, "This is the only operator who can open this workspace",
            "Removing them would make every encrypted case in it permanently unreadable. Add "
            "another operator first.");
    }
    const Entry removed = *found;
    entries_.erase(found);
    if (auto status = save(); !status) {
        entries_.push_back(removed);
        return status;
    }
    logInfo(kComponent, "Operator removed from the workspace keyring",
            JsonValue::object().set("username", username));
    return Status::success();
}

bool Keyring::hasOperator(const std::string& username) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&username](const Entry& entry) { return entry.username == username; });
}

std::vector<std::string> Keyring::operators() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const Entry& entry : entries_) names.push_back(entry.username);
    return names;
}

}  // namespace trace
