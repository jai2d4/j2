#include "reporting/bundle_verifier.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <system_error>

#include "core/security/file_hasher.h"

namespace trace {
namespace {

/// Parses a `sha256sum` listing: 64 hex characters, two spaces, then the path.
/// Deliberately strict — a line TRACE cannot read is reported, never skipped.
Result<std::vector<std::pair<std::string, std::string>>> readChecksumFile(
    const std::filesystem::path& path) {
    using ResultType = Result<std::vector<std::pair<std::string, std::string>>>;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return ResultType::failure(ErrorCode::NotFound, "Could not read " + path.string());
    }

    std::vector<std::pair<std::string, std::string>> entries;
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() < 67 || line.compare(64, 2, "  ") != 0) {
            return ResultType::failure(ErrorCode::InvalidArgument,
                                       path.filename().string() + " line " +
                                           std::to_string(lineNumber) + " is not a sha256 entry");
        }
        const std::string digest = line.substr(0, 64);
        if (digest.find_first_not_of("0123456789abcdef") != std::string::npos) {
            return ResultType::failure(ErrorCode::InvalidArgument,
                                       path.filename().string() + " line " +
                                           std::to_string(lineNumber) + " has a malformed digest");
        }
        entries.emplace_back(digest, line.substr(66));
    }
    return ResultType::success(std::move(entries));
}

}  // namespace

int BundleVerification::failureCount() const {
    return static_cast<int>(std::count_if(files.begin(), files.end(),
                                          [](const FileVerification& f) { return !f.matches; })) +
           static_cast<int>(unlistedFiles.size());
}

std::string BundleVerifier::verifyDocument() {
    return
        "# Verifying this exhibit bundle\n"
        "\n"
        "This bundle was produced by TRACE. You do not need TRACE, a network connection, or\n"
        "any scripting language to check it — only the SHA-256 tool your operating system\n"
        "already ships with.\n"
        "\n"
        "Every file is listed with the digest of the bytes as written. If a digest does not\n"
        "match, the file has changed since export.\n"
        "\n"
        "There are **three** checks, and the third matters as much as the other two. A\n"
        "digest check proves every listed file is intact; it cannot tell you whether a file\n"
        "has been *added*, because an extra file is simply not on the list. Counting the\n"
        "files closes that gap.\n"
        "\n"
        "## Linux and macOS\n"
        "\n"
        "Run all three from the bundle's own directory.\n"
        "\n"
        "```bash\n"
        "# 1. the manifests themselves\n"
        "sha256sum -c MANIFEST.sha256\n"
        "\n"
        "# 2. every file the manifests list\n"
        "sha256sum -c MANIFEST.checksums\n"
        "\n"
        "# 3. nothing has been added that no manifest vouches for\n"
        "listed=$(wc -l < MANIFEST.checksums)\n"
        "present=$(find . -type f ! -name 'MANIFEST.*' | wc -l)\n"
        "if [ \"$listed\" -eq \"$present\" ]; then\n"
        "    echo \"file count OK ($listed files)\"\n"
        "else\n"
        "    echo \"MISMATCH: $present files present, $listed listed\"\n"
        "    find . -type f ! -name 'MANIFEST.*' | sed 's|^\\./||' | sort > /tmp/present.$$\n"
        "    cut -c67- MANIFEST.checksums | sort > /tmp/listed.$$\n"
        "    echo \"not vouched for by any manifest:\"\n"
        "    comm -23 /tmp/present.$$ /tmp/listed.$$\n"
        "    rm -f /tmp/present.$$ /tmp/listed.$$\n"
        "fi\n"
        "```\n"
        "\n"
        "On macOS use `shasum -a 256 -c` in place of `sha256sum -c`.\n"
        "\n"
        "Every line of checks 1 and 2 must report `OK`, and check 3 must report `file count\n"
        "OK`. A single `FAILED` means that file is not the file that was exported. A count\n"
        "mismatch means something is in the bundle that was not exported with it.\n"
        "\n"
        "## Windows (PowerShell)\n"
        "\n"
        "```powershell\n"
        "# 1 and 2: every file the manifests list\n"
        "Get-Content .\\MANIFEST.checksums | ForEach-Object {\n"
        "    $expected = $_.Substring(0, 64)\n"
        "    $path     = $_.Substring(66)\n"
        "    $actual   = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()\n"
        "    if ($actual -eq $expected) { Write-Host \"OK      $path\" }\n"
        "    else                       { Write-Host \"FAILED  $path\" -ForegroundColor Red }\n"
        "}\n"
        "\n"
        "# 3: nothing has been added\n"
        "$listed  = (Get-Content .\\MANIFEST.checksums | ForEach-Object { $_.Substring(66) })\n"
        "$present = (Get-ChildItem -Recurse -File | Where-Object { $_.Name -notlike 'MANIFEST.*' } |\n"
        "            ForEach-Object { (Resolve-Path -Relative $_.FullName).Substring(2) -replace '\\\\','/' })\n"
        "$extra = Compare-Object $listed $present | Where-Object { $_.SideIndicator -eq '=>' }\n"
        "if ($extra) { Write-Host \"NOT VOUCHED FOR:\" -ForegroundColor Red; $extra.InputObject }\n"
        "else        { Write-Host \"file count OK\" }\n"
        "```\n"
        "\n"
        "## What each file is\n"
        "\n"
        "| File | Contents |\n"
        "|---|---|\n"
        "| `MANIFEST.sha256` | Digests of the two manifests below. Check this first. |\n"
        "| `MANIFEST.checksums` | Every content file and its digest, in `sha256sum` format. |\n"
        "| `MANIFEST.json` | The same list, plus who exported it and when. |\n"
        "| `REPORT.html` | The report. Open it in any browser; it needs no network. |\n"
        "| `exhibits/` | The frames and clips cited by the report. |\n"
        "| `provenance/` | The evidence, analysis runs and detections cited, as JSON. |\n"
        "| `audit/` | The audit events supporting everything above. |\n"
        "\n"
        "## What verification does and does not prove\n"
        "\n"
        "A matching digest proves the file is byte-for-byte what TRACE wrote at export time.\n"
        "Check 3 proves nothing has been added alongside it.\n"
        "\n"
        "None of this proves who exported the bundle, or that the export was authorised.\n"
        "This is an integrity manifest, not a digital signature. Nothing in this bundle is\n"
        "signed, and a manifest can be regenerated by anyone holding the files — what it\n"
        "establishes is that the bundle is internally consistent and unchanged since it was\n"
        "written, not who wrote it.\n";
}

Result<BundleVerification> BundleVerifier::verify(const std::filesystem::path& bundleRoot) {
    using ResultType = Result<BundleVerification>;
    BundleVerification report;

    std::error_code ec;
    if (!std::filesystem::is_directory(bundleRoot, ec)) {
        return ResultType::failure(ErrorCode::NotFound,
                                   "No bundle directory at " + bundleRoot.string());
    }

    // ---- 1. the manifests themselves ------------------------------------
    auto top = readChecksumFile(bundleRoot / "MANIFEST.sha256");
    if (!top) return ResultType(top.error());

    for (const auto& [expected, name] : top.value()) {
        auto actual = hashFile(bundleRoot / name);
        if (!actual) {
            report.problem = "Could not read " + name;
            return ResultType::success(std::move(report));
        }
        if (actual.value() != expected) {
            report.problem = name + " does not match the digest recorded in MANIFEST.sha256";
            return ResultType::success(std::move(report));
        }
        if (name == "MANIFEST.json") report.manifestSha256 = expected;
    }
    report.manifestIntact = true;

    // ---- 2. every listed file -------------------------------------------
    auto listed = readChecksumFile(bundleRoot / "MANIFEST.checksums");
    if (!listed) return ResultType(listed.error());

    std::map<std::string, bool> seen;
    bool allMatch = true;
    for (const auto& [expected, relative] : listed.value()) {
        FileVerification file;
        file.path = relative;
        file.expectedSha256 = expected;
        seen[relative] = true;

        const auto absolute = bundleRoot / std::filesystem::path(relative);
        if (!std::filesystem::exists(absolute, ec)) {
            file.problem = "missing from the bundle";
            allMatch = false;
            report.files.push_back(std::move(file));
            continue;
        }
        file.present = true;

        auto actual = hashFile(absolute);
        if (!actual) {
            file.problem = "could not be read";
            allMatch = false;
            report.files.push_back(std::move(file));
            continue;
        }
        file.actualSha256 = actual.take();
        file.matches = file.actualSha256 == file.expectedSha256;
        if (!file.matches) {
            file.problem = "content has changed since export";
            allMatch = false;
        }
        report.files.push_back(std::move(file));
    }
    report.allFilesMatch = allMatch;

    // ---- 3. anything present but unlisted --------------------------------
    // A file nobody vouched for is as much a problem as a file that changed.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(bundleRoot, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string relative =
            std::filesystem::relative(entry.path(), bundleRoot).generic_string();
        if (relative == "MANIFEST.json" || relative == "MANIFEST.checksums" ||
            relative == "MANIFEST.sha256") {
            continue;
        }
        if (seen.find(relative) == seen.end()) report.unlistedFiles.push_back(relative);
    }
    report.noUnlistedFiles = report.unlistedFiles.empty();

    return ResultType::success(std::move(report));
}

}  // namespace trace
