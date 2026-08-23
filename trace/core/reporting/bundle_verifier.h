#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/common/result.h"

namespace trace {

/// Outcome for one file named in a bundle's manifest.
struct FileVerification {
    std::string path;
    std::string expectedSha256;
    std::string actualSha256;
    bool present = false;
    bool matches = false;
    std::string problem;
};

/// What re-checking a bundle found.
///
/// Manifest integrity is a separate gate on purpose: if MANIFEST.checksums itself has
/// been altered, the per-file results below it prove nothing, and reporting them as
/// passes would be worse than reporting nothing.
struct BundleVerification {
    bool manifestIntact = false;
    bool allFilesMatch = false;
    bool noUnlistedFiles = true;
    std::string manifestSha256;
    std::vector<FileVerification> files;
    std::vector<std::string> unlistedFiles;
    std::string problem;

    bool passed() const { return manifestIntact && allFilesMatch && noUnlistedFiles; }
    int failureCount() const;
};

/// Re-verifies an exhibit bundle from its own manifests.
///
/// This is the same guarantee VERIFY.md gives a third party, offered inside the
/// application and computed by the same hashing code — so the two can never disagree.
class BundleVerifier {
public:
    static Result<BundleVerification> verify(const std::filesystem::path& bundleRoot);

    /// The verification instructions written into every bundle.
    static std::string verifyDocument();
};

}  // namespace trace
