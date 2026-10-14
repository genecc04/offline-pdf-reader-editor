#include "MainWindow.h"
#include "ThumbnailPane.h"
#include "SearchPane.h"
#include "BookmarkPane.h"

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPdfDocument>
#include <QPdfPageNavigator>
#include <QPdfView>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolBar>
#include <QDockWidget>
#include <QInputDialog>
#include <QLineEdit>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupViewer();
    setupThumbnailPane();
    setupSearchPane();
    setupBookmarkPane();
    setupMenus();
    setupToolbar();

    setWindowTitle("PDF Reader");
    resize(900, 700);
}

void MainWindow::setupViewer()
{
    m_document = new QPdfDocument(this);

    m_view = new QPdfView(this);
    m_view->setDocument(m_document);
    m_view->setPageMode(QPdfView::PageMode::MultiPage);
    m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    setCentralWidget(m_view);
}

void MainWindow::setupMenus()
{
    QMenu *fileMenu = menuBar()->addMenu("&File");
    QAction *openAction = fileMenu->addAction("&Open...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

    QMenu *viewMenu = menuBar()->addMenu("&View");
    QAction *togglePages = m_pagesDock->toggleViewAction();
    togglePages->setShortcut(Qt::Key_F4);
    viewMenu->addAction(togglePages);
    QAction *toggleBookmarks = m_bookmarksDock->toggleViewAction();
    toggleBookmarks->setShortcut(Qt::CTRL | Qt::Key_B);
    viewMenu->addAction(toggleBookmarks);

    QAction *findAction = viewMenu->addAction("&Find...");
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, [this]() {
        m_searchDock->show();
        m_searchDock->raise();
        m_search->focusSearchBox();
    });
}

void MainWindow::setupToolbar()
{
    QToolBar *toolbar = addToolBar("Navigation");
    toolbar->setMovable(false);

    // Page navigation
    QAction *prevAction = toolbar->addAction("< Prev");
    QAction *nextAction = toolbar->addAction("Next >");

    m_pageBox = new QSpinBox(this);
    m_pageBox->setRange(0, 0);
    toolbar->addWidget(m_pageBox);
    m_pageCountLabel = new QLabel(" / 0 ", this);
    toolbar->addWidget(m_pageCountLabel);

    toolbar->addSeparator();

    // Zoom
    QAction *zoomOutAction = toolbar->addAction("Zoom -");
    QAction *zoomInAction = toolbar->addAction("Zoom +");
    QAction *fitWidthAction = toolbar->addAction("Fit width");
    QAction *fitPageAction = toolbar->addAction("Fit page");

    // Wiring
    QPdfPageNavigator *nav = m_view->pageNavigator();

    connect(prevAction, &QAction::triggered, this, [this, nav]() {
        goToPage(nav->currentPage() - 1);
    });
    connect(nextAction, &QAction::triggered, this, [this, nav]() {
        goToPage(nav->currentPage() + 1);
    });
    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) {
        goToPage(value - 1);
    });
    connect(nav, &QPdfPageNavigator::currentPageChanged, this, [this](int page) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setValue(page + 1);
    });
    connect(m_document, &QPdfDocument::pageCountChanged, this, [this](int count) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setRange(count > 0 ? 1 : 0, count);
        m_pageCountLabel->setText(QString(" / %1 ").arg(count));
    });

    connect(zoomInAction, &QAction::triggered, this, [this]() {
        m_view->setZoomMode(QPdfView::ZoomMode::Custom);
        m_view->setZoomFactor(m_view->zoomFactor() * 1.25);
    });
    connect(zoomOutAction, &QAction::triggered, this, [this]() {
        m_view->setZoomMode(QPdfView::ZoomMode::Custom);
        m_view->setZoomFactor(m_view->zoomFactor() / 1.25);
    });
    connect(fitWidthAction, &QAction::triggered, this, [this]() {
        m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    });
    connect(fitPageAction, &QAction::triggered, this, [this]() {
        m_view->setZoomMode(QPdfView::ZoomMode::FitInView);
    });
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Open PDF", QString(), "PDF files (*.pdf)");
    if (path.isEmpty())
        return;

    m_document->setPassword(QString());
    QPdfDocument::Error error = m_document->load(path);

    bool firstAttempt = true;
    while (error == QPdfDocument::Error::IncorrectPassword) {
        const QString prompt = firstAttempt
            ? "This PDF is protected. Enter its password:"
            : "Incorrect password. Try again:";
        firstAttempt = false;

        bool ok = false;
        const QString password = QInputDialog::getText(
            this, "Password required", prompt, QLineEdit::Password, QString(), &ok);
        if (!ok) {
            setWindowTitle("PDF Reader");
            return;
        }

        m_document->setPassword(password);
        error = m_document->load(path);
    }

    if (error != QPdfDocument::Error::None) {
        setWindowTitle("PDF Reader");
        QMessageBox::warning(this, "Error", "Could not open that PDF.");
        return;
    }
    setWindowTitle(QFileInfo(path).fileName() + " - PDF Reader");
}

void MainWindow::goToPage(int page)
{
    if (page < 0 || page >= m_document->pageCount())
        return;
    QPdfPageNavigator *nav = m_view->pageNavigator();
    nav->jump(page, {}, nav->currentZoom());
}

void MainWindow::setupThumbnailPane()
{
    m_thumbnails = new ThumbnailPane(this);
    m_thumbnails->setDocument(m_document);

    m_pagesDock = new QDockWidget("Pages", this);
    m_pagesDock->setWidget(m_thumbnails);
    m_pagesDock->setFeatures(QDockWidget::DockWidgetClosable |
                             QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, m_pagesDock);

    connect(m_thumbnails, &ThumbnailPane::pageRequested, this, &MainWindow::goToPage);
    connect(m_view->pageNavigator(), &QPdfPageNavigator::currentPageChanged,
            m_thumbnails, &ThumbnailPane::setCurrentPage);
}

void MainWindow::setupSearchPane()
{
    m_search = new SearchPane(this);
    m_search->setDocument(m_document);
    m_view->setSearchModel(m_search->searchModel());

    m_searchDock = new QDockWidget("Search", this);
    m_searchDock->setWidget(m_search);
    m_searchDock->setFeatures(QDockWidget::DockWidgetClosable |
                              QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, m_searchDock);
    tabifyDockWidget(m_pagesDock, m_searchDock);
    m_pagesDock->raise();

    connect(m_search, &SearchPane::resultSelected, this,
            [this](int index, int page, QPointF location) {
                QPdfPageNavigator *nav = m_view->pageNavigator();
                nav->jump(page, location, nav->currentZoom());
                m_view->setCurrentSearchResultIndex(index);
            });
}

void MainWindow::setupBookmarkPane()
{
    m_bookmarks = new BookmarkPane(this);
    m_bookmarks->setDocument(m_document);

    m_bookmarksDock = new QDockWidget("Bookmarks", this);
    m_bookmarksDock->setWidget(m_bookmarks);
    m_bookmarksDock->setFeatures(QDockWidget::DockWidgetClosable |
                                 QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, m_bookmarksDock);
    tabifyDockWidget(m_pagesDock, m_bookmarksDock);   // third tab on the left
    m_pagesDock->raise();

    connect(m_bookmarks, &BookmarkPane::bookmarkSelected, this,
            [this](int page, QPointF location) {
                QPdfPageNavigator *nav = m_view->pageNavigator();
                nav->jump(page, location, nav->currentZoom());
            });
}