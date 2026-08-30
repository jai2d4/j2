#include "core/security/workspace_keys.h"

namespace trace {
namespace {

/// Domain separation. A key derived for a case must not coincide with one
/// derived for anything else the master key is ever used for.
constexpr const char* kCaseKeyInfo = "trace-case-v1";

}  // namespace

void WorkspaceKeys::markEncrypted() {
    std::lock_guard<std::mutex> guard(mutex_);
    encrypted_ = true;
}

void WorkspaceKeys::unlock(crypto::SecretKey masterKey) {
    std::lock_guard<std::mutex> guard(mutex_);
    masterKey_ = std::move(masterKey);
    encrypted_ = true;
}

void WorkspaceKeys::lock() {
    std::lock_guard<std::mutex> guard(mutex_);
    masterKey_.reset();
}

bool WorkspaceKeys::unlocked() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return masterKey_.has_value();
}

bool WorkspaceKeys::encrypted() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return encrypted_;
}

Result<crypto::SecretKey> WorkspaceKeys::masterKey() const {
    using ResultType = Result<crypto::SecretKey>;
    std::lock_guard<std::mutex> guard(mutex_);
    if (!masterKey_) {
        return ResultType::failure(ErrorCode::NotFound, "This workspace is locked");
    }
    return ResultType::success(*masterKey_);
}

Result<crypto::SecretKey> WorkspaceKeys::caseKey(const std::string& caseId) const {
    using ResultType = Result<crypto::SecretKey>;
    if (caseId.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "A case key needs the case it belongs to");
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (!masterKey_) {
        return ResultType::failure(ErrorCode::NotFound, "This workspace is locked");
    }
    // The case identifier is a UUID: unique per case, and not secret, which is
    // all a KDF salt has to be.
    const std::vector<std::uint8_t> salt(caseId.begin(), caseId.end());
    return masterKey_->deriveSubkey(salt, kCaseKeyInfo);
}

Result<CaseKeyHandle> caseKeyFor(const WorkspaceKeys& keys, const std::string& caseId) {
    using ResultType = Result<CaseKeyHandle>;
    if (!keys.encrypted()) {
        return ResultType::success(CaseKeyHandle());
    }
    if (!keys.unlocked()) {
        // Returning an empty handle here would write evidence in the clear into
        // a workspace whose operator has been told it is encrypted. That is the
        // one outcome worth failing an ingestion over.
        return ResultType::failure(
            ErrorCode::PermissionDenied, "This workspace is encrypted and locked",
            "Sign in before adding to or reading from it. Nothing has been written.");
    }
    auto key = keys.caseKey(caseId);
    if (!key) return ResultType(key.error());
    return ResultType::success(CaseKeyHandle(key.take()));
}

}  // namespace trace
