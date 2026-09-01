#include "ui/app/application_context.h"

#include "media/ffmpeg/hardware_decode.h"

#include "ai/detection/detection_provider_registry.h"
#include "ui/reports/qt_document_renderer.h"
#include "core/common/logging.h"
#include "core/common/uuid.h"
#include "core/database/migrations.h"
#include "core/repositories/user_repository.h"
#include "core/security/user_context.h"
#include "media/ffmpeg/media_probe.h"
#include "trace/trace_version.h"

namespace trace::ui {
namespace {
constexpr const char* kComponent = "application";
}

ApplicationContext::ApplicationContext(QObject* parent) : QObject(parent) {}

ApplicationContext::~ApplicationContext() { shutdown(); }

Status ApplicationContext::initialise(const std::filesystem::path& dataRoot) {
    layout_ = std::make_unique<StorageLayout>(dataRoot);
    if (auto status = layout_->ensureDataRoot(); !status) return status;

    Logger::instance().setLogDirectory(layout_->logsDirectory());
    logInfo(kComponent, "TRACE starting",
            JsonValue::object()
                .set("version", kApplicationVersion)
                .set("phase", kPhase)
                .set("data_root", dataRoot.string()));

    workspace_ = WorkspaceService::inspect(dataRoot);
    if (workspace_.encrypted) {
        if (!workspace_.buildSupportsEncryption) {
            return Status::failure(
                ErrorCode::Unsupported, "This workspace is encrypted and this build cannot open it",
                "The evidence is intact. A build of TRACE with encryption support will open it.");
        }
        if (!keys_->unlocked()) {
            // Opening what can be opened and failing on each case afterwards
            // would look like a broken workspace rather than a locked one.
            return Status::failure(
                ErrorCode::PermissionDenied, "This workspace is encrypted and has not been unlocked",
                "Sign in with an operator who has access to it.");
        }
    }

    auto opened = workspace_.databaseEncrypted
                      ? Database::openEncrypted(layout_->databasePath(), keys_->masterKey().take())
                      : Database::open(layout_->databasePath());
    if (!opened) return Status(opened.error());
    database_ = opened.take();

    auto migrated = MigrationRunner::applyAll(*database_);
    if (!migrated) return Status(migrated.error());
    const MigrationReport report = migrated.take();

    auditService_ = std::make_shared<AuditService>(database_);
    caseService_ = std::make_unique<CaseService>(database_, *layout_, auditService_);
    evidenceService_ = std::make_unique<EvidenceService>(
        database_, *layout_, auditService_, std::make_shared<FFmpegMetadataExtractor>(), keys_);
    integrityService_ =
        std::make_unique<IntegrityService>(database_, *layout_, auditService_, keys_);
    annotationService_ = std::make_unique<AnnotationService>(database_, auditService_);
    derivedAssetService_ = std::make_shared<DerivedAssetService>(database_, *layout_, auditService_);
    frameExportService_ = std::make_unique<FrameExportService>(*layout_, derivedAssetService_);
    waveformService_ = std::make_unique<WaveformService>(*layout_, derivedAssetService_);
    captureService_ = std::make_unique<CaptureService>(*layout_, *evidenceService_,
                                                      derivedAssetService_, auditService_);
    analysisService_ = std::make_shared<AnalysisService>(database_, auditService_);
    modelManager_ = std::make_unique<ModelManager>(ModelManager::defaultModelDirectory(dataRoot));
    reportService_ = std::make_unique<ReportService>(
        *layout_, database_, auditService_, *caseService_, *evidenceService_, *analysisService_,
        *annotationService_, derivedAssetService_);
    reportService_->setDocumentRenderer(std::make_shared<QtDocumentRenderer>());
    settingsService_ = std::make_unique<SettingsService>(database_, auditService_);

    // Providers announce themselves once per process. Listing them loads no
    // model and acquires no device — that happens when a run starts.
    registerBuiltinDetectionProviders();

    if (auto status = settingsService_->ensureDefaults(); !status) return status;
    Logger::instance().setLevel(settingsService_->logLevel());

    authService_ = std::make_unique<AuthService>(database_, auditService_);

    // Register the workstation operator so the audit rows written before anyone
    // signs in — the migration, the application start — name somebody rather
    // than nobody.
    //
    // This row carries no credential and so cannot be signed into. It also does
    // not confer a session: UserContext is replaced only by AuthService::signIn,
    // which is what stops the identity the operating system reports from being
    // mistaken for an authenticated one.
    UserRepository users(database_);
    UserAccount account = UserContext::current().account();
    if (auto stored = users.upsert(account); stored) {
        UserContext::current().setAccount(stored.take());
    }

    if (!report.appliedVersions.empty()) {
        AuditRecord record;
        record.action = AuditAction::DatabaseMigrated;
        record.description = "Database schema updated to version " +
                             std::to_string(report.currentVersion);
        JsonValue versions = JsonValue::array();
        for (int version : report.appliedVersions) versions.push(version);
        record.details = JsonValue::object()
                             .set("from_version", report.previousVersion)
                             .set("to_version", report.currentVersion)
                             .set("applied", versions);
        auditService_->record(record);
    }

    AuditRecord record;
    record.action = AuditAction::ApplicationStarted;
    record.description = std::string("TRACE ") + kApplicationVersion + " started";
    record.details = JsonValue::object()
                         .set("version", kApplicationVersion)
                         .set("phase", kPhase)
                         .set("data_root", dataRoot.string())
                         .set("schema_version", report.currentVersion);
    auditService_->record(record);

    initialised_ = true;
    return Status::success();
}

std::string ApplicationContext::decodeDevice() const {
    if (!isInitialised()) return {};
    if (!settingsService_->getBool(settings_keys::kHardwareAcceleration, false)) return {};
    const auto usable = hwaccel::availableDevices();
    if (usable.empty()) return {};
    return usable.front().name;
}

WorkspaceState ApplicationContext::inspectWorkspace(const std::filesystem::path& dataRoot) {
    return WorkspaceService::inspect(dataRoot);
}

Status ApplicationContext::unlockWorkspace(const std::filesystem::path& dataRoot,
                                           const std::string& username,
                                           const std::string& password) {
    return WorkspaceService::unlock(dataRoot, username, password, *keys_);
}

void ApplicationContext::shutdown() {
    if (!initialised_) return;
    initialised_ = false;

    if (auditService_ != nullptr) {
        AuditRecord record;
        record.action = AuditAction::ApplicationStopped;
        record.description = "TRACE closed";
        auditService_->record(record);
    }
    logInfo(kComponent, "TRACE shutting down");

    frameExportService_.reset();
    waveformService_.reset();
    captureService_.reset();
    // Drop the key before anything else: from here on nothing more should be
    // decrypted, and holding it after the services that use it have gone is
    // just a key sitting in memory for no reason.
    keys_->lock();
    authService_.reset();
    settingsService_.reset();
    reportService_.reset();
    modelManager_.reset();
    analysisService_.reset();
    derivedAssetService_.reset();
    annotationService_.reset();
    integrityService_.reset();
    evidenceService_.reset();
    caseService_.reset();
    auditService_.reset();
    database_.reset();
    Logger::instance().shutdown();
}

AnalysisPipeline ApplicationContext::makeAnalysisPipeline() const {
    return AnalysisPipeline(analysisService_, *modelManager_, *layout_);
}

QString ApplicationContext::currentCaseNumber() const {
    return currentCase_ ? QString::fromStdString(currentCase_->caseNumber) : QString();
}

Status ApplicationContext::openCase(const QString& caseId) {
    auto opened = caseService_->openCase(caseId.toStdString());
    if (!opened) return Status(opened.error());
    currentCase_ = opened.take();
    currentEvidence_.reset();
    emit caseOpened();
    emit currentEvidenceChanged();
    emit evidenceChanged();
    emit auditChanged();
    return Status::success();
}

void ApplicationContext::closeCase() {
    currentCase_.reset();
    currentEvidence_.reset();
    emit caseClosed();
    emit currentEvidenceChanged();
    emit evidenceChanged();
}

void ApplicationContext::refreshCurrentCase() {
    if (!currentCase_) return;
    auto found = caseService_->findById(currentCase_->id);
    if (found && found.value().has_value()) {
        currentCase_ = *found.take();
        emit currentCaseUpdated();
    }
}

void ApplicationContext::setCurrentEvidence(const std::optional<Evidence>& evidence) {
    currentEvidence_ = evidence;
    emit currentEvidenceChanged();
}

void ApplicationContext::refreshCurrentEvidence() {
    if (!currentEvidence_) return;
    auto found = evidenceService_->findById(currentEvidence_->id);
    if (found && found.value().has_value()) {
        currentEvidence_ = *found.take();
        emit currentEvidenceChanged();
    }
}

// Late notifications are dropped rather than sent to panels that would then
// query services this context has already released.
void ApplicationContext::notifyEvidenceChanged() {
    if (initialised_) emit evidenceChanged();
}
void ApplicationContext::notifyBookmarksChanged() {
    if (initialised_) emit bookmarksChanged();
}
void ApplicationContext::notifyAnnotationsChanged() {
    if (initialised_) emit annotationsChanged();
}
void ApplicationContext::notifyDerivedAssetsChanged() {
    if (initialised_) emit derivedAssetsChanged();
}
void ApplicationContext::notifyAuditChanged() {
    if (initialised_) emit auditChanged();
}
void ApplicationContext::notifyCasesChanged() {
    if (initialised_) emit casesChanged();
}
void ApplicationContext::notifyAnalysisRunsChanged() {
    if (initialised_) emit analysisRunsChanged();
}
void ApplicationContext::notifyDetectionsChanged() {
    if (initialised_) emit detectionsChanged();
}
void ApplicationContext::notifyReportsChanged() {
    if (initialised_) emit reportsChanged();
}

}  // namespace trace::ui
