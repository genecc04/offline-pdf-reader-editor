#include "MainWindow.h"
#include "ThumbnailPane.h"
#include "SearchPane.h"
#include "BookmarkPane.h"
#include "PdfEditor.h"

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
#include <QStatusBar>
#include <QCloseEvent>
#include <QFile>

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

    m_saveAsAction = fileMenu->addAction("Save &As...");
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    m_saveAsAction->setEnabled(false);   // nothing to save until a PDF is open
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::saveAs);

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

    toolbar->addSeparator();

    // Editing
    m_rotateLeftAction = toolbar->addAction("Rotate left");
    m_rotateRightAction = toolbar->addAction("Rotate right");
    m_rotateLeftAction->setEnabled(false);    // enabled once a PDF is open for editing
    m_rotateRightAction->setEnabled(false);
    connect(m_rotateLeftAction, &QAction::triggered, this, [this]() { rotateCurrentPage(-90); });
    connect(m_rotateRightAction, &QAction::triggered, this, [this]() { rotateCurrentPage(90); });

    // Delete
    m_deletePageAction = toolbar->addAction("Delete page");
    m_deletePageAction->setEnabled(false);
    connect(m_deletePageAction, &QAction::triggered, this, &MainWindow::deleteCurrentPage);

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
    if (!confirmDiscard())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, "Open PDF", QString(), "PDF files (*.pdf)");
    if (path.isEmpty())
        return;

    m_editor.reset();
    m_currentPath.clear();
    m_viewPath.clear();
    m_password.clear();
    m_displayName.clear();
    m_modified = false;
    m_saveAsAction->setEnabled(false);
    m_rotateLeftAction->setEnabled(false);
    m_rotateRightAction->setEnabled(false);
    m_thumbnails->setReorderEnabled(false);
    m_deletePageAction->setEnabled(false);

    m_document->setPassword(QString());   // don't reuse the previous file's password
    QPdfDocument::Error error = m_document->load(path);

    QString usedPassword;
    bool firstAttempt = true;
    while (error == QPdfDocument::Error::IncorrectPassword) {
        const QString prompt = firstAttempt
            ? "This PDF is protected. Enter its password:"
            : "Incorrect password. Try again:";
        firstAttempt = false;

        bool ok = false;
        const QString password = QInputDialog::getText(
            this, "Password required", prompt, QLineEdit::Password, QString(), &ok);
        if (!ok) {                         // user pressed Cancel
            updateTitle();
            return;
        }

        m_document->setPassword(password);
        usedPassword = password;
        error = m_document->load(path);
    }

    if (error != QPdfDocument::Error::None) {
        updateTitle();
        QMessageBox::warning(this, "Error", "Could not open that PDF.");
        return;
    }

    m_currentPath = path;
    m_viewPath = path;
    m_password = usedPassword;
    m_displayName = QFileInfo(path).fileName();

    // Open the same file for editing. Viewing still works if this fails.
    m_editor = std::make_unique<PdfEditor>();
    if (m_editor->open(path, m_password)) {
        m_saveAsAction->setEnabled(true);
        m_rotateLeftAction->setEnabled(true);
        m_rotateRightAction->setEnabled(true);
        m_thumbnails->setReorderEnabled(true);
        m_deletePageAction->setEnabled(true);
    } else {
        statusBar()->showMessage("Editing is not available for this file.", 5000);
        m_editor.reset();
    }
    updateTitle();
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

    // Queued: the move reloads the document, which must not happen in the middle of a drop.
    connect(m_thumbnails, &ThumbnailPane::pageMoveRequested, this,
            &MainWindow::movePage, Qt::QueuedConnection);
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

bool MainWindow::saveAs()
{
    if (!m_editor)
        return false;

    const QString dest = QFileDialog::getSaveFileName(
        this, "Save PDF As", QString(), "PDF files (*.pdf)");
    if (dest.isEmpty())
        return false;

    // The editor still reads from the original file, so never overwrite it.
    if (QFileInfo(dest) == QFileInfo(m_currentPath)) {
        QMessageBox::warning(this, "Save As",
                             "Please choose a different file name than the open document.");
        return false;
    }

    if (!m_editor->saveAs(dest)) {
        QMessageBox::warning(this, "Save failed", m_editor->lastError());
        return false;
    }

    setModified(false);
    statusBar()->showMessage("Saved " + QFileInfo(dest).fileName(), 4000);
    return true;
}

void MainWindow::rotateCurrentPage(int degrees)
{
    if (!m_editor)
        return;

    const int page = m_view->pageNavigator()->currentPage();

    if (!m_editor->rotatePage(page, degrees)) {
        QMessageBox::warning(this, "Rotate failed", m_editor->lastError());
        return;
    }
    refreshAfterEdit(page);
}

void MainWindow::showFile(const QString &path, int page)
{
    const QString previous = m_viewPath;

    m_document->setPassword(m_password);
    if (m_document->load(path) != QPdfDocument::Error::None) {
        QMessageBox::warning(this, "Error", "Could not display the edited document.");
        return;
    }
    m_viewPath = path;

    if (previous.startsWith(m_tempDir.path()))
        QFile::remove(previous);

    goToPage(page);
    m_thumbnails->setCurrentPage(page);
}

bool MainWindow::confirmDiscard()
{
    if (!m_modified)
        return true;

    const auto answer = QMessageBox::question(
        this, "Unsaved changes",
        "You have unsaved changes. Save them first?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Save)
        return saveAs();
    return answer == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (confirmDiscard())
        event->accept();
    else
        event->ignore();
}

void MainWindow::setModified(bool modified)
{
    m_modified = modified;
    updateTitle();
}

void MainWindow::updateTitle()
{
    if (m_displayName.isEmpty())
        setWindowTitle("PDF Reader");
    else
        setWindowTitle(m_displayName + (m_modified ? " *" : "") + " - PDF Reader");
}

bool MainWindow::refreshAfterEdit(int page)
{
    // Write the edited document to a fresh temp file and show that.
    const QString newPath = m_tempDir.filePath(
        QString("view-%1.pdf").arg(++m_editCount));
    if (!m_editor->saveAs(newPath)) {
        QMessageBox::warning(this, "Edit failed", m_editor->lastError());
        return false;
    }

    showFile(newPath, page);
    setModified(true);
    return true;
}

void MainWindow::deleteCurrentPage()
{
    if (!m_editor)
        return;

    const int pageCount = m_document->pageCount();
    if (pageCount <= 1) {
        QMessageBox::information(this, "Delete page",
                                 "A PDF must keep at least one page.");
        return;
    }

    const int page = m_view->pageNavigator()->currentPage();
    const auto answer = QMessageBox::question(
        this, "Delete page",
        QString("Delete page %1 of %2?").arg(page + 1).arg(pageCount),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    if (!m_editor->deletePage(page)) {
        QMessageBox::warning(this, "Delete failed", m_editor->lastError());
        return;
    }

    // One page fewer: if the last page was deleted, land on the new last page.
    refreshAfterEdit(qMin(page, pageCount - 2));
}

void MainWindow::movePage(int from, int to)
{
    if (!m_editor)
        return;

    if (!m_editor->movePage(from, to)) {
        QMessageBox::warning(this, "Move failed", m_editor->lastError());
        return;
    }
    refreshAfterEdit(to);            // stay on the page that was moved
}