#pragma once
#include <QMainWindow>
#include <QString>
#include <QTemporaryDir>
#include <memory>

#include "PdfEditor.h"

class QPdfDocument;
class QPdfView;
class QSpinBox;
class QLabel;
class QDockWidget;
class ThumbnailPane;
class SearchPane;
class BookmarkPane;
class QAction;
class QCloseEvent;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

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
    bool saveAs();
    void goToPage(int page);
    void showFile(const QString &path, int page);

    // editing
    void rotateCurrentPage(int degrees);
    void movePage(int from, int to);
    void extractPages();
    void deleteCurrentPage();
    bool refreshAfterEdit(int page);

    // document state
    bool confirmDiscard();
    void setModified(bool modified);
    void updateTitle();

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

    QAction *m_saveAsAction = nullptr;
    QAction *m_rotateLeftAction = nullptr;
    QAction *m_rotateRightAction = nullptr;
    QAction *m_extractAction = nullptr;
    QAction *m_deletePageAction = nullptr;

    std::unique_ptr<PdfEditor> m_editor;
    QTemporaryDir m_tempDir;
    QString m_currentPath;
    QString m_viewPath;
    QString m_displayName;
    QString m_password;
    int m_editCount = 0;
    bool m_modified = false;
};