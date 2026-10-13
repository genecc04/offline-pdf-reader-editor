#include "MainWindow.h"
#include "ThumbnailPane.h"

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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupViewer();
    setupThumbnailPane();
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
    QString path = QFileDialog::getOpenFileName(
        this, "Open PDF", QString(), "PDF files (*.pdf)");
    if (path.isEmpty())
        return;

    if (m_document->load(path) != QPdfDocument::Error::None) {
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

    // thumbnail click -> viewer, and viewer scroll -> thumbnail highlight
    connect(m_thumbnails, &ThumbnailPane::pageRequested, this, &MainWindow::goToPage);
    connect(m_view->pageNavigator(), &QPdfPageNavigator::currentPageChanged,
            m_thumbnails, &ThumbnailPane::setCurrentPage);
}