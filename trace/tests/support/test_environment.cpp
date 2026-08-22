#include "tests/support/test_environment.h"

#include <atomic>
#include <stdexcept>

#include "core/common/logging.h"
#include "core/common/uuid.h"
#include "core/database/migrations.h"
#include "ai/detection/models/model_manager.h"
#include "media/ffmpeg/media_probe.h"

namespace trace::testing {
namespace {
std::atomic<int> counter{0};
}

TemporaryDirectory::TemporaryDirectory(const std::string& prefix) {
    const auto base = std::filesystem::temp_directory_path();
    path_ = base / (prefix + "-" + uuidToCompact(generateUuid()).substr(0, 12) + "-" +
                    std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(path_);
}

TemporaryDirectory::~TemporaryDirectory() {
    std::error_code ec;
    // Managed originals are made read-only at ingestion; restore write
    // permission so the scratch directory can actually be removed.
    for (auto it = std::filesystem::recursive_directory_iterator(
             path_, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code permissionError;
        std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add, permissionError);
    }
    std::filesystem::remove_all(path_, ec);
}

std::filesystem::path sampleVideoPath() {
#ifdef TRACE_TEST_FIXTURES_DIR
    const std::filesystem::path fixture = std::filesystem::path(TRACE_TEST_FIXTURES_DIR) / "sample.mp4";
#else
    const std::filesystem::path fixture = "tests/fixtures/sample.mp4";
#endif
    if (!std::filesystem::exists(fixture)) {
        throw std::runtime_error("Sample fixture missing: " + fixture.string());
    }
    return fixture;
}

TestStack TestStack::create(const std::filesystem::path& dataRoot, bool withMediaExtractor) {
    TestStack stack;
    stack.layout = std::make_unique<StorageLayout>(dataRoot);
    if (auto status = stack.layout->ensureDataRoot(); !status) {
        throw std::runtime_error("Unable to prepare test data root: " + status.error().toString());
    }

    auto opened = Database::open(stack.layout->databasePath());
    if (!opened) throw std::runtime_error("Unable to open test database: " + opened.error().toString());
    stack.database = opened.take();

    auto migrated = MigrationRunner::applyAll(*stack.database);
    if (!migrated) throw std::runtime_error("Migrations failed: " + migrated.error().toString());

    stack.audit = std::make_shared<AuditService>(stack.database);
    stack.cases = std::make_unique<CaseService>(stack.database, *stack.layout, stack.audit);
    stack.evidence = std::make_unique<EvidenceService>(
        stack.database, *stack.layout, stack.audit,
        withMediaExtractor ? std::make_shared<FFmpegMetadataExtractor>() : nullptr);
    stack.integrity = std::make_unique<IntegrityService>(stack.database, *stack.layout, stack.audit);
    stack.annotations = std::make_unique<AnnotationService>(stack.database, stack.audit);
    stack.derivedAssets =
        std::make_shared<DerivedAssetService>(stack.database, *stack.layout, stack.audit);
    stack.analysis = std::make_shared<AnalysisService>(stack.database, stack.audit);
    return stack;
}

std::filesystem::path modelsDirectory() {
#ifdef TRACE_TEST_MODELS_DIR
    return std::filesystem::path(TRACE_TEST_MODELS_DIR);
#else
    return std::filesystem::path("models");
#endif
}

std::optional<std::filesystem::path> pedestrianVideoPath() {
#ifdef TRACE_TEST_FIXTURES_DIR
    const std::filesystem::path candidate =
        std::filesystem::path(TRACE_TEST_FIXTURES_DIR) / "external" / "vtest.avi";
#else
    const std::filesystem::path candidate = "tests/fixtures/external/vtest.avi";
#endif
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) return candidate;
    return std::nullopt;
}

bool realDetectionModelAvailable() {
    const auto descriptor = ModelManager::describe("yolox-tiny");
    if (!descriptor) return false;
    ModelManager manager(modelsDirectory());
    auto validation = manager.validate(*descriptor);
    return validation.ok() && validation.value().usable();
}

}  // namespace trace::testing
