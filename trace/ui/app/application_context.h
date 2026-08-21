#pragma once

#include <QObject>
#include <QString>

#include <filesystem>
#include <memory>
#include <optional>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/case.h"
#include "core/models/evidence.h"
#include "core/services/annotation_service.h"
#include "core/services/audit_service.h"
#include "core/services/case_service.h"
#include "core/services/derived_asset_service.h"
#include "core/services/evidence_service.h"
#include "core/services/integrity_service.h"
#include "core/settings/settings_service.h"
#include "core/storage/storage_layout.h"
#include "media/thumbnails/frame_export_service.h"

namespace trace::ui {

/// Wiring for the whole application: opens the database, runs migrations,
/// constructs the services and holds the currently selected case and evidence.
///
/// Widgets receive a pointer to this and never construct services or touch SQL
/// themselves, which is what keeps the UI layer free of domain logic.
class ApplicationContext : public QObject {
    Q_OBJECT

public:
    explicit ApplicationContext(QObject* parent = nullptr);
    ~ApplicationContext() override;

    /// Opens (creating if needed) the data directory, database and services.
    Status initialise(const std::filesystem::path& dataRoot);
    /// Records shutdown and closes the database.
    void shutdown();

    bool isInitialised() const { return initialised_; }
    const StorageLayout& layout() const { return *layout_; }
    std::shared_ptr<Database> database() const { return database_; }

    CaseService& cases() const { return *caseService_; }
    EvidenceService& evidence() const { return *evidenceService_; }
    IntegrityService& integrity() const { return *integrityService_; }
    AnnotationService& annotations() const { return *annotationService_; }
    DerivedAssetService& derivedAssets() const { return *derivedAssetService_; }
    FrameExportService& frameExports() const { return *frameExportService_; }
    AuditService& audit() const { return *auditService_; }
    SettingsService& settings() const { return *settingsService_; }

    const std::optional<Case>& currentCase() const { return currentCase_; }
    const std::optional<Evidence>& currentEvidence() const { return currentEvidence_; }
    QString currentCaseNumber() const;

    /// Opens a case (records the audit event) and makes it current.
    Status openCase(const QString& caseId);
    void closeCase();
    /// Re-reads the current case row after an edit.
    void refreshCurrentCase();

    void setCurrentEvidence(const std::optional<Evidence>& evidence);
    void refreshCurrentEvidence();

    /// Notifications for panels that need to re-query.
    void notifyEvidenceChanged();
    void notifyBookmarksChanged();
    void notifyAnnotationsChanged();
    void notifyDerivedAssetsChanged();
    void notifyAuditChanged();
    void notifyCasesChanged();

signals:
    void casesChanged();
    void caseOpened();
    void caseClosed();
    void currentCaseUpdated();
    void evidenceChanged();
    void currentEvidenceChanged();
    void bookmarksChanged();
    void annotationsChanged();
    void derivedAssetsChanged();
    void auditChanged();
    void statusMessage(const QString& message, int timeoutMs);

private:
    bool initialised_ = false;
    std::shared_ptr<Database> database_;
    std::unique_ptr<StorageLayout> layout_;
    std::shared_ptr<AuditService> auditService_;
    std::unique_ptr<CaseService> caseService_;
    std::unique_ptr<EvidenceService> evidenceService_;
    std::unique_ptr<IntegrityService> integrityService_;
    std::unique_ptr<AnnotationService> annotationService_;
    std::shared_ptr<DerivedAssetService> derivedAssetService_;
    std::unique_ptr<FrameExportService> frameExportService_;
    std::unique_ptr<SettingsService> settingsService_;

    std::optional<Case> currentCase_;
    std::optional<Evidence> currentEvidence_;
};

}  // namespace trace::ui
