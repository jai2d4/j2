#include "ui/app/application_context.h"

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

    auto opened = Database::open(layout_->databasePath());
    if (!opened) return Status(opened.error());
    database_ = opened.take();

    auto migrated = MigrationRunner::applyAll(*database_);
    if (!migrated) return Status(migrated.error());
    const MigrationReport report = migrated.take();

    auditService_ = std::make_shared<AuditService>(database_);
    caseService_ = std::make_unique<CaseService>(database_, *layout_, auditService_);
    evidenceService_ = std::make_unique<EvidenceService>(
        database_, *layout_, auditService_, std::make_shared<FFmpegMetadataExtractor>());
    integrityService_ = std::make_unique<IntegrityService>(database_, *layout_, auditService_);
    annotationService_ = std::make_unique<AnnotationService>(database_, auditService_);
    derivedAssetService_ = std::make_shared<DerivedAssetService>(database_, *layout_, auditService_);
    frameExportService_ = std::make_unique<FrameExportService>(*layout_, derivedAssetService_);
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

    // Register the workstation operator so audit rows can be joined to an
    // account once multi-user deployments arrive.
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
