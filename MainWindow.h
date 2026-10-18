#pragma once
#include <QList>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

#include "EditHistory.h"
#include "PdfEditor.h"

class QAction;
class QCloseEvent;
class QDockWidget;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QMenu;
class QPdfDocument;
class QPdfView;
class QSpinBox;
class BookmarkPane;
class SearchPane;
class ThumbnailPane;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openPath(const QString &path);   // asks to save unsaved changes first

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // setup
    void setupViewer();
    void setupThumbnailPane();
    void setupSearchPane();
    void setupBookmarkPane();
    void setupMenus();
    void setupToolbar();

    // file and viewing
    void openFile();
    void loadDocument(const QString &path);
    bool saveAs();
    void goToPage(int page);
    bool showFile(const QString &path, int page, bool reportErrors = true);
    void addRecentFile(const QString &path);
    void updateRecentMenu();
    void showAbout();

    // editing
    void rotateSelectedPages(int degrees);
    void deleteSelectedPages();
    void movePage(int from, int to);
    void extractPages();
    void insertPages();
    bool refreshAfterEdit(int page, const QList<int> &reselect = {});
    QList<int> targetPages() const;
    void undo();
    void redo();
    void stepHistory(const QString &path);

    // document state
    bool confirmDiscard();
    void updateTitle();
    void updateActions();
    bool reopenEditor(const QString &path);
    void removeSnapshots(const QStringList &paths);
    bool isTempFile(const QString &path) const;

    QPdfDocument *m_document = nullptr;
    QPdfView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;
    QLabel *m_pageCountLabel = nullptr;
    ThumbnailPane *m_thumbnails = nullptr;
    QDockWidget *m_pagesDock = nullptr;
    SearchPane *m_search = nullptr;
    QDockWidget *m_searchDock = nullptr;
    BookmarkPane *m_bookmarks = nullptr;
    QDockWidget *m_bookmarksDock = nullptr;
    QMenu *m_recentMenu = nullptr;

    QAction *m_saveAsAction = nullptr;
    QAction *m_extractAction = nullptr;
    QAction *m_insertAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_rotateLeftAction = nullptr;
    QAction *m_rotateRightAction = nullptr;
    QAction *m_deletePageAction = nullptr;

    std::unique_ptr<PdfEditor> m_editor;
    EditHistory m_history;
    QTemporaryDir m_tempDir;
    QString m_currentPath;
    QString m_displayName;
    QString m_password;
    int m_editCount = 0;
};