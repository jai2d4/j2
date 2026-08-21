#pragma once

#include <QWidget>

#include <memory>
#include <optional>

#include "core/models/evidence.h"
#include "core/models/media_metadata.h"

class QFormLayout;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;

namespace trace::ui {

class ApplicationContext;
class BackgroundTask;

/// Right-hand inspector: what this evidence is, what its digest is, whether it
/// still matches, and what has been derived from it.
///
/// Fields the container did not provide read "Not available" — nothing here is
/// inferred or filled in with a plausible default.
class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(ApplicationContext* context, QWidget* parent = nullptr);
    ~InspectorPanel() override;

    void showEvidence(const std::optional<Evidence>& evidence);
    void refresh();
    void verifyIntegrity();

signals:
    void statusMessage(const QString& message, int timeoutMs);
    void integrityChecked(bool verified);

private:
    void buildUi();
    void updateIntegritySection();
    void updateMetadataSection();
    void updateDerivedAssets();

    ApplicationContext* context_ = nullptr;
    std::optional<Evidence> evidence_;
    std::optional<MediaMetadata> metadata_;
    std::unique_ptr<BackgroundTask> verifyTask_;

    QLabel* evidenceNumber_ = nullptr;
    QLabel* evidenceFilename_ = nullptr;
    QFormLayout* evidenceForm_ = nullptr;
    QFormLayout* mediaForm_ = nullptr;
    QLabel* integrityBadge_ = nullptr;
    QLabel* hashLabel_ = nullptr;
    QLabel* lastVerifiedLabel_ = nullptr;
    QLabel* integrityDetail_ = nullptr;
    QPushButton* verifyButton_ = nullptr;
    QProgressBar* verifyProgress_ = nullptr;
    QTreeWidget* streamTree_ = nullptr;
    QTreeWidget* derivedTree_ = nullptr;
    QPlainTextEdit* rawMetadata_ = nullptr;
    QPushButton* rawToggle_ = nullptr;
};

}  // namespace trace::ui
