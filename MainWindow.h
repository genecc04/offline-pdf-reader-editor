#pragma once
#include <QMainWindow>

class QPdfDocument;
class QPdfView;
class QSpinBox;
class QLabel;
class QDockWidget;
class ThumbnailPane;
class SearchPane;
class BookmarkPane;
class QAction;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void setupViewer();
    void setupThumbnailPane();
    void setupSearchPane();
    void setupBookmarkPane();
    void setupMenus();
    void setupToolbar();
    void openFile();
    void goToPage(int page);
    void saveAs();
    

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
    QString m_currentPath;
    QString m_password;
};