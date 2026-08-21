#pragma once

#include <QWidget>

#include <vector>

#include "core/models/bookmark.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace trace::ui {

class ApplicationContext;

/// Bookmarks for the evidence in the viewer: add, edit, delete, jump.
class BookmarksPanel : public QWidget {
    Q_OBJECT

public:
    explicit BookmarksPanel(ApplicationContext* context, QWidget* parent = nullptr);

    void refresh();
    /// Creates a bookmark at `positionUs`, prompting for title and note.
    void addBookmarkAt(qint64 positionUs, qint64 frameNumber);
    const std::vector<Bookmark>& bookmarks() const { return bookmarks_; }
    void selectBookmark(const QString& bookmarkId);

signals:
    void jumpRequested(qint64 positionUs);
    void bookmarksChanged();
    /// The window supplies the playhead position and frame number.
    void addRequested();

private:
    std::optional<Bookmark> selected() const;
    void editSelected();
    void deleteSelected();

    ApplicationContext* context_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    std::vector<Bookmark> bookmarks_;
};

}  // namespace trace::ui
