#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/security/crypto.h"

namespace trace {

/// One file inside an exhibit bundle, with the digest of the bytes actually written.
struct ManifestEntry {
    std::string path;  ///< relative to the bundle root, forward slashes
    std::string sha256;
    std::int64_t sizeBytes = 0;
};

/// Identity of an export, written into MANIFEST.json.
struct BundleIdentity {
    std::string traceVersion;
    std::string exportedAtUtc;
    std::string caseId;
    std::string caseNumber;
    std::string caseTitle;
    std::string reportId;
    std::string reportTitle;
    std::string exportedBy;
    bool includedUnconfirmed = false;
};

/// Writes the exhibit bundle and the manifests that make it verifiable.
///
/// Verification deliberately needs no JSON parser and no scripting language: the file
/// list is also written in `sha256sum -c` format, so a third party checks an entire
/// bundle with one command that ships with their operating system.
///
///   MANIFEST.sha256      digests of the two manifests below
///   MANIFEST.checksums   every content file, in sha256sum format
///   MANIFEST.json        the same list plus export identity, machine-readable
///
/// This class contains no Qt: `core` is the headless domain, and a bundle must be
/// writable by a future CLI or service with no display.
class BundleWriter {
public:
    /// The statement every report carries, verbatim and not editable from the UI.
    static const char* limitationsStatement();

    explicit BundleWriter(std::filesystem::path bundleRoot);

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path exhibitsDirectory() const { return root_ / "exhibits"; }

    /// Creates the directory layout. Fails if the destination cannot be written.
    Status createLayout();

    /// Copies a produced artefact (frame, clip) into `exhibits/` under `name`.
    /// Copies a file from managed storage into the bundle's exhibits directory.
    ///
    /// `key` decrypts a source that is an encrypted container. **The bundle
    /// always holds plaintext**, and that is not an oversight: a bundle exists
    /// to be handed to somebody who does not have TRACE and checked with
    /// `sha256sum`, so an encrypted exhibit would defeat the entire point of
    /// producing one. Protecting a bundle is a question of how it is
    /// transported, which is outside this software — `docs/ENCRYPTION.md` §6
    /// says so rather than letting anyone assume otherwise.
    Result<ManifestEntry> addExhibit(const std::filesystem::path& source, const std::string& name,
                                     const crypto::SecretKey* key = nullptr);
    /// Writes a text document at `relativePath`, creating parents as needed.
    Result<ManifestEntry> addTextFile(const std::string& relativePath, const std::string& contents);
    /// Records a file a caller wrote into the bundle itself — used where producing the
    /// file needs a facility this module does not have, such as a text engine for PDF.
    /// The file must already exist inside the bundle root.
    Result<ManifestEntry> addProducedFile(const std::string& relativePath);

    /// Writes MANIFEST.checksums, MANIFEST.json and MANIFEST.sha256 over everything
    /// written so far, and returns the digest of MANIFEST.json.
    Result<std::string> finalise(const BundleIdentity& identity);

    const std::vector<ManifestEntry>& entries() const { return entries_; }

private:
    Result<ManifestEntry> recordFile(const std::filesystem::path& absolute,
                                     const std::string& relativePath);

    std::filesystem::path root_;
    std::vector<ManifestEntry> entries_;
};

}  // namespace trace
