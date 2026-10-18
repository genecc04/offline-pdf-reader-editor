#include "MainWindow.h"
#include "ThumbnailPane.h"
#include "SearchPane.h"
#include "BookmarkPane.h"
#include "PdfEditor.h"
#include "PageRangeDialog.h"

#include <QAction>
#include <QDir>
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
    updateActions();

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
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::saveAs);

    m_extractAction = fileMenu->addAction("&Extract pages...");
    connect(m_extractAction, &QAction::triggered, this, &MainWindow::extractPages);

    m_insertAction = fileMenu->addAction("&Insert pages from PDF...");
    connect(m_insertAction, &QAction::triggered, this, &MainWindow::insertPages);

    QMenu *editMenu = menuBar()->addMenu("&Edit");
    m_undoAction = editMenu->addAction("&Undo");
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undo);

    m_redoAction = editMenu->addAction("&Redo");
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redo);

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
    connect(m_rotateLeftAction, &QAction::triggered, this, [this]() { rotateSelectedPages(-90); });
    connect(m_rotateRightAction, &QAction::triggered, this, [this]() { rotateSelectedPages(90); });

    m_deletePageAction = toolbar->addAction("Delete pages");
    connect(m_deletePageAction, &QAction::triggered, this, &MainWindow::deleteSelectedPages);

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
    tabifyDockWidget(m_pagesDock, m_bookmarksDock);
    m_pagesDock->raise();

    connect(m_bookmarks, &BookmarkPane::bookmarkSelected, this,
            [this](int page, QPointF location) {
                QPdfPageNavigator *nav = m_view->pageNavigator();
                nav->jump(page, location, nav->currentZoom());
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
    m_document->close();
    removeSnapshots(m_history.states());
    m_history.clear();
    m_currentPath.clear();
    m_password.clear();
    m_displayName.clear();
    updateActions();

    m_document->setPassword(QString());
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
        if (!ok) {
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
    m_password = usedPassword;
    m_displayName = QFileInfo(path).fileName();
    m_history.reset(path);

    if (!m_tempDir.isValid() || !reopenEditor(path))
        statusBar()->showMessage("Editing is not available for this file.", 5000);

    updateActions();
    updateTitle();
}

void MainWindow::goToPage(int page)
{
    if (page < 0 || page >= m_document->pageCount())
        return;
    QPdfPageNavigator *nav = m_view->pageNavigator();
    nav->jump(page, {}, nav->currentZoom());
}

bool MainWindow::showFile(const QString &path, int page, bool reportErrors)
{
    m_document->close();
    m_document->setPassword(m_password);

    const QPdfDocument::Error error = m_document->load(path);
    if (error != QPdfDocument::Error::None) {
        if (reportErrors) {
            QString reason;
            switch (error) {
            case QPdfDocument::Error::IncorrectPassword:
                reason = "incorrect password"; break;
            case QPdfDocument::Error::InvalidFileFormat:
                reason = "invalid file format"; break;
            case QPdfDocument::Error::UnsupportedSecurityScheme:
                reason = "unsupported security scheme"; break;
            case QPdfDocument::Error::FileNotFound:
                reason = "file not found"; break;
            default:
                reason = "unknown error"; break;
            }
            QMessageBox::warning(this, "Error",
                                 QString("Could not display the document (%1).").arg(reason));
        }
        return false;
    }

    const int target = qBound(0, page, m_document->pageCount() - 1);
    goToPage(target);
    m_thumbnails->setCurrentPage(target);
    return true;
}

bool MainWindow::saveAs()
{
    if (!m_editor)
        return false;

    const QString dest = QFileDialog::getSaveFileName(
        this, "Save PDF As", QString(), "PDF files (*.pdf)");
    if (dest.isEmpty())
        return false;

    if (QFileInfo(dest) == QFileInfo(m_currentPath)
        || QFileInfo(dest) == QFileInfo(m_history.current())) {
        QMessageBox::warning(this, "Save As",
                             "Please choose a different file name than the open document.");
        return false;
    }

    PdfEditor saver;
    if (!saver.open(m_history.current(), m_password) || !saver.saveAs(dest, true)) {
        QMessageBox::warning(this, "Save failed", saver.lastError());
        return false;
    }

    m_history.markSaved();
    updateTitle();
    statusBar()->showMessage("Saved " + QFileInfo(dest).fileName(), 4000);
    return true;
}

bool MainWindow::refreshAfterEdit(int page, const QList<int> &reselect)
{
    if (!m_tempDir.isValid()) {
        QMessageBox::warning(this, "Edit failed", "Could not create a temporary folder.");
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    const QString newPath = m_tempDir.filePath(
        QString("state-%1.pdf").arg(++m_editCount));
    if (!m_editor->saveAs(newPath)) {
        QMessageBox::warning(this, "Edit failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    if (!showFile(newPath, page)) {
        QFile::remove(newPath);
        showFile(m_history.current(), page, false);
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    if (!reselect.isEmpty())
        m_thumbnails->selectPages(reselect);

    removeSnapshots(m_history.push(newPath));
    reopenEditor(newPath);
    updateActions();
    updateTitle();
    return true;
}

QList<int> MainWindow::targetPages() const
{
    QList<int> pages = m_thumbnails->selectedPages();
    if (pages.isEmpty())
        pages << m_view->pageNavigator()->currentPage();
    return pages;
}

void MainWindow::rotateSelectedPages(int degrees)
{
    if (!m_editor)
        return;

    const QList<int> pages = targetPages();
    const int viewPage = m_view->pageNavigator()->currentPage();

    for (int page : pages) {
        if (!m_editor->rotatePage(page, degrees)) {
            QMessageBox::warning(this, "Rotate failed", m_editor->lastError());
            reopenEditor(m_history.current());
            updateActions();
            return;
        }
    }

    if (refreshAfterEdit(viewPage, pages))
        statusBar()->showMessage(QString("Rotated %1 page(s)").arg(pages.size()), 3000);
}

void MainWindow::deleteSelectedPages()
{
    if (!m_editor)
        return;

    const QList<int> pages = targetPages();
    const int pageCount = m_document->pageCount();
    if (pages.size() >= pageCount) {
        QMessageBox::information(this, "Delete pages",
                                 "A PDF must keep at least one page.");
        return;
    }

    for (int i = pages.size() - 1; i >= 0; --i) {
        if (!m_editor->deletePage(pages.at(i))) {
            QMessageBox::warning(this, "Delete failed", m_editor->lastError());
            reopenEditor(m_history.current());
            updateActions();
            return;
        }
    }

    const int landing = qMin(pages.first(), pageCount - pages.size() - 1);
    if (refreshAfterEdit(landing))
        statusBar()->showMessage(QString("Deleted %1 page(s)").arg(pages.size()), 3000);
}

void MainWindow::movePage(int from, int to)
{
    if (!m_editor)
        return;

    if (!m_editor->movePage(from, to)) {
        QMessageBox::warning(this, "Move failed", m_editor->lastError());
        return;
    }
    refreshAfterEdit(to);
}

void MainWindow::extractPages()
{
    if (!m_editor)
        return;

    const int pageCount = m_document->pageCount();
    const int current = m_view->pageNavigator()->currentPage();

    PageRangeDialog dialog(pageCount, current, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString dest = QFileDialog::getSaveFileName(
        this, "Save extracted pages as", QString(), "PDF files (*.pdf)");
    if (dest.isEmpty())
        return;

    if (QFileInfo(dest) == QFileInfo(m_currentPath)
        || QFileInfo(dest) == QFileInfo(m_history.current())) {
        QMessageBox::warning(this, "Extract pages",
                             "Please choose a different file name than the open document.");
        return;
    }

    PdfEditor extractor;
    if (!extractor.open(m_history.current(), m_password)
        || !extractor.keepOnlyPages(dialog.firstPage(), dialog.lastPage())
        || !extractor.saveAs(dest, true)) {
        QMessageBox::warning(this, "Extract failed", extractor.lastError());
        return;
    }

    statusBar()->showMessage("Extracted pages to " + QFileInfo(dest).fileName(), 4000);
}

void MainWindow::insertPages()
{
    if (!m_editor)
        return;

    const QString source = QFileDialog::getOpenFileName(
        this, "Choose the PDF to insert", QString(), "PDF files (*.pdf)");
    if (source.isEmpty())
        return;

    const int pageCount = m_document->pageCount();
    const int current = m_view->pageNavigator()->currentPage();

    bool ok = false;
    const int after = QInputDialog::getInt(
        this, "Insert pages",
        QString("Insert after page (0 = at the start, %1 = at the end):").arg(pageCount),
        current + 1, 0, pageCount, 1, &ok);
    if (!ok)
        return;

    if (!m_editor->insertPagesFrom(source, after)) {
        QMessageBox::warning(this, "Insert failed", m_editor->lastError());
        return;
    }

    refreshAfterEdit(after);
}

void MainWindow::undo()
{
    stepHistory(m_history.undo());
}

void MainWindow::redo()
{
    stepHistory(m_history.redo());
}

void MainWindow::stepHistory(const QString &path)
{
    if (path.isEmpty())
        return;

    const int page = m_view->pageNavigator()->currentPage();
    if (!showFile(path, page))
        return;

    if (!reopenEditor(path))
        QMessageBox::warning(this, "Editing", "Editing is no longer available for this file.");

    updateActions();
    updateTitle();
}

bool MainWindow::confirmDiscard()
{
    if (!m_history.isModified())
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

void MainWindow::updateTitle()
{
    if (m_displayName.isEmpty())
        setWindowTitle("PDF Reader");
    else
        setWindowTitle(m_displayName + (m_history.isModified() ? " *" : "") + " - PDF Reader");
}

void MainWindow::updateActions()
{
    const bool editable = (m_editor != nullptr);

    m_saveAsAction->setEnabled(editable);
    m_extractAction->setEnabled(editable);
    m_insertAction->setEnabled(editable);
    m_rotateLeftAction->setEnabled(editable);
    m_rotateRightAction->setEnabled(editable);
    m_deletePageAction->setEnabled(editable);
    m_thumbnails->setReorderEnabled(editable);

    m_undoAction->setEnabled(editable && m_history.canUndo());
    m_redoAction->setEnabled(editable && m_history.canRedo());
}

bool MainWindow::reopenEditor(const QString &path)
{
    m_editor = std::make_unique<PdfEditor>();
    if (m_editor->open(path, m_password))
        return true;

    m_editor.reset();
    return false;
}

void MainWindow::removeSnapshots(const QStringList &paths)
{
    for (const QString &path : paths) {
        if (isTempFile(path))
            QFile::remove(path);
    }
}

bool MainWindow::isTempFile(const QString &path) const
{
    if (!m_tempDir.isValid())
        return false;
    const QString folder = QDir::cleanPath(m_tempDir.path()) + '/';
    return QDir::cleanPath(path).startsWith(folder);
}