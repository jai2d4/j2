#pragma once

#include <QWidget>

#include <optional>
#include <vector>

#include "core/models/annotation.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace trace::ui {

class ApplicationContext;

/// Timestamped notes for the evidence in the viewer.
class AnnotationsPanel : public QWidget {
    Q_OBJECT

public:
    explicit AnnotationsPanel(ApplicationContext* context, QWidget* parent = nullptr);

    void refresh();
    void addAnnotationAt(qint64 positionUs, qint64 frameNumber);
    const std::vector<Annotation>& annotations() const { return annotations_; }

signals:
    void jumpRequested(qint64 positionUs);
    void annotationsChanged();
    /// The window supplies the playhead position and frame number.
    void addRequested();

private:
    std::optional<Annotation> selected() const;
    void editSelected();
    void deleteSelected();

    ApplicationContext* context_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    std::vector<Annotation> annotations_;
};

}  // namespace trace::ui
