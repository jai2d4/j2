#include "reporting/bundle_writer.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "core/common/json.h"
#include "core/common/logging.h"
#include "core/security/file_hasher.h"

namespace trace {
namespace {
constexpr const char* kComponent = "reporting";

Status writeText(const std::filesystem::path& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Status::failure(ErrorCode::IoError, "Could not write " + path.string());
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    if (!out) {
        return Status::failure(ErrorCode::IoError, "Failed while writing " + path.string());
    }
    return Status::success();
}

}  // namespace

const char* BundleWriter::limitationsStatement() {
    return
        "This document records what TRACE observed and what was done to the evidence it "
        "describes. Detections are machine-produced observations of a visual class with a "
        "confidence score; they are not identifications and are not conclusions. Where a "
        "detection is marked confirmed, that records only that the named analyst agreed with "
        "it at the time stated. Interpretation of this material is the analyst's and the "
        "court's.";
}

BundleWriter::BundleWriter(std::filesystem::path bundleRoot) : root_(std::move(bundleRoot)) {}

Status BundleWriter::createLayout() {
    std::error_code ec;
    if (std::filesystem::exists(root_, ec)) {
        return Status::failure(ErrorCode::AlreadyExists,
                               "A bundle already exists at " + root_.string() +
                                   ". Exporting would overwrite it; choose another destination.");
    }
    for (const auto& directory : {root_, exhibitsDirectory(), root_ / "provenance",
                                  root_ / "audit"}) {
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return Status::failure(ErrorCode::IoError,
                                   "Could not create " + directory.string() + ": " + ec.message());
        }
    }
    return Status::success();
}

Result<ManifestEntry> BundleWriter::recordFile(const std::filesystem::path& absolute,
                                               const std::string& relativePath) {
    auto hashed = hashFile(absolute);
    if (!hashed) return Result<ManifestEntry>(hashed.error());

    std::error_code ec;
    const auto size = std::filesystem::file_size(absolute, ec);

    ManifestEntry entry;
    entry.path = relativePath;
    entry.sha256 = hashed.take();
    entry.sizeBytes = ec ? 0 : static_cast<std::int64_t>(size);
    entries_.push_back(entry);
    return Result<ManifestEntry>::success(std::move(entry));
}

Result<ManifestEntry> BundleWriter::addExhibit(const std::filesystem::path& source,
                                               const std::string& name) {
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        return Result<ManifestEntry>::failure(ErrorCode::NotFound,
                                              "Exhibit source is missing: " + source.string());
    }
    const auto destination = exhibitsDirectory() / name;
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Result<ManifestEntry>::failure(
            ErrorCode::IoError, "Could not copy exhibit into the bundle: " + ec.message());
    }
    return recordFile(destination, "exhibits/" + name);
}

Result<ManifestEntry> BundleWriter::addTextFile(const std::string& relativePath,
                                                const std::string& contents) {
    const auto absolute = root_ / std::filesystem::path(relativePath);
    if (auto status = writeText(absolute, contents); !status) {
        return Result<ManifestEntry>(status.error());
    }
    return recordFile(absolute, relativePath);
}

Result<ManifestEntry> BundleWriter::addProducedFile(const std::string& relativePath) {
    const auto absolute = root_ / std::filesystem::path(relativePath);
    std::error_code ec;
    if (!std::filesystem::exists(absolute, ec)) {
        return Result<ManifestEntry>::failure(
            ErrorCode::NotFound, "Nothing was produced at " + relativePath + " to record");
    }
    return recordFile(absolute, relativePath);
}

Result<std::string> BundleWriter::finalise(const BundleIdentity& identity) {
    // Deterministic order, so two exports of the same selection differ only where they
    // genuinely differ.
    std::sort(entries_.begin(), entries_.end(),
              [](const ManifestEntry& a, const ManifestEntry& b) { return a.path < b.path; });

    // ------------------------------------------------- MANIFEST.checksums
    // sha256sum's own format: digest, two spaces, path. `sha256sum -c` consumes this
    // directly, so verifying a bundle needs nothing but a standard system tool.
    std::string checksums;
    for (const auto& entry : entries_) {
        checksums += entry.sha256 + "  " + entry.path + "\n";
    }
    if (auto status = writeText(root_ / "MANIFEST.checksums", checksums); !status) {
        return Result<std::string>(status.error());
    }

    // ------------------------------------------------------ MANIFEST.json
    JsonValue manifest = JsonValue::object();
    manifest.set("trace_version", identity.traceVersion)
        .set("exported_at_utc", identity.exportedAtUtc)
        .set("case_id", identity.caseId)
        .set("case_number", identity.caseNumber)
        .set("case_title", identity.caseTitle)
        .set("report_id", identity.reportId)
        .set("report_title", identity.reportTitle)
        .set("exported_by", identity.exportedBy)
        .set("included_unconfirmed_detections", identity.includedUnconfirmed)
        .set("file_count", static_cast<std::int64_t>(entries_.size()))
        .set("limitations", limitationsStatement());

    JsonValue files = JsonValue::array();
    for (const auto& entry : entries_) {
        files.push(JsonValue::object()
                       .set("path", entry.path)
                       .set("sha256", entry.sha256)
                       .set("size_bytes", entry.sizeBytes));
    }
    manifest.set("files", files);

    const std::string manifestText = manifest.dump(2) + "\n";
    if (auto status = writeText(root_ / "MANIFEST.json", manifestText); !status) {
        return Result<std::string>(status.error());
    }

    // ---------------------------------------------------- MANIFEST.sha256
    auto manifestDigest = hashFile(root_ / "MANIFEST.json");
    if (!manifestDigest) return Result<std::string>(manifestDigest.error());
    auto checksumsDigest = hashFile(root_ / "MANIFEST.checksums");
    if (!checksumsDigest) return Result<std::string>(checksumsDigest.error());

    const std::string manifestSha = manifestDigest.take();
    const std::string top = manifestSha + "  MANIFEST.json\n" + checksumsDigest.take() +
                            "  MANIFEST.checksums\n";
    if (auto status = writeText(root_ / "MANIFEST.sha256", top); !status) {
        return Result<std::string>(status.error());
    }

    logInfo(kComponent, "Exhibit bundle written",
            JsonValue::object()
                .set("bundle", root_.string())
                .set("files", static_cast<std::int64_t>(entries_.size()))
                .set("manifest_sha256", manifestSha));

    return Result<std::string>::success(manifestSha);
}

}  // namespace trace
