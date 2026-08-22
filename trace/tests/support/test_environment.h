#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/database/database.h"
#include "core/services/analysis_service.h"
#include "core/services/annotation_service.h"
#include "core/services/audit_service.h"
#include "core/services/case_service.h"
#include "core/services/derived_asset_service.h"
#include "core/services/evidence_service.h"
#include "core/services/integrity_service.h"
#include "core/storage/storage_layout.h"

namespace trace::testing {

/// A scratch directory that removes itself. Tests never touch a real data
/// directory, and never modify the checked-in fixtures.
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::string& prefix = "trace-test");
    ~TemporaryDirectory();
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& child) const { return path_ / child; }

private:
    std::filesystem::path path_;
};

/// Path of the checked-in sample video. Tests copy it before doing anything
/// destructive: the fixture itself is treated as read-only.
std::filesystem::path sampleVideoPath();

/// Directory holding detection model artefacts for development runs.
std::filesystem::path modelsDirectory();

/// A clip containing people and vehicles, used by the tests that exercise real
/// inference. It is fetched by scripts/fetch_test_media.sh rather than
/// committed, so it may be absent — those tests skip themselves and say so.
std::optional<std::filesystem::path> pedestrianVideoPath();

/// True when the YOLOX-Tiny artefact is installed and passes validation.
bool realDetectionModelAvailable();

/// A fully wired stack over a temporary data directory: database, migrations,
/// storage layout and every service, including the real FFmpeg extractor.
struct TestStack {
    std::shared_ptr<Database> database;
    std::unique_ptr<StorageLayout> layout;
    std::shared_ptr<AuditService> audit;
    std::unique_ptr<CaseService> cases;
    std::unique_ptr<EvidenceService> evidence;
    std::unique_ptr<IntegrityService> integrity;
    std::unique_ptr<AnnotationService> annotations;
    std::shared_ptr<DerivedAssetService> derivedAssets;
    std::shared_ptr<AnalysisService> analysis;

    static TestStack create(const std::filesystem::path& dataRoot, bool withMediaExtractor = true);
};

}  // namespace trace::testing
