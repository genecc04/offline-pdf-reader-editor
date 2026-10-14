#pragma once
#include <QMainWindow>

class QPdfDocument;
class QPdfView;
class QSpinBox;
class QLabel;
class QDockWidget;
class ThumbnailPane;
class SearchPane;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void setupViewer();
    void setupThumbnailPane();
    void setupSearchPane();
    void setupMenus();
    void setupToolbar();
    void openFile();
    void goToPage(int page);
    

    QPdfDocument *m_document = nullptr;
    QPdfView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;
    QLabel *m_pageCountLabel = nullptr;
    ThumbnailPane *m_thumbnails = nullptr;
    QDockWidget *m_pagesDock = nullptr;
    SearchPane *m_search = nullptr;
    QDockWidget *m_searchDock = nullptr;
};