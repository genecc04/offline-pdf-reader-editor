#include "MainWindow.h"
#include "ThumbnailPane.h"
#include "SearchPane.h"
#include "BookmarkPane.h"
#include "PdfEditor.h"
#include "PageRangeDialog.h"
#include "ColorMenuButton.h"
#include "TextRenderer.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPdfDocument>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QActionGroup>
#include <QFontComboBox>
#include <QBuffer>
#include <QImageReader>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <algorithm>

namespace {

    constexpr int kMaxRecentFiles = 8;
    constexpr int kMaxImageSide = 2400;

    QString droppedPdf(const QMimeData *mime)
    {
        if (!mime->hasUrls())
            return QString();
        const QList<QUrl> urls = mime->urls();
        for (const QUrl &url : urls) {
            if (url.isLocalFile() && url.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive))
                return url.toLocalFile();
        }
        return QString();
    }

    QString createSiblingTempPath(const QString &target, const QString &purpose,
                                  QString *error)
    {
        const QFileInfo targetInfo(target);
        const QString pattern = QDir(targetInfo.absolutePath()).filePath(
            targetInfo.fileName() + ".pdfapp-" + purpose + "-XXXXXX");
        QTemporaryFile temp(pattern);
        if (!temp.open()) {
            *error = "Could not create a temporary file beside the PDF ("
                     + temp.errorString() + ").";
            return QString();
        }

        temp.setAutoRemove(false);
        const QString path = temp.fileName();
        temp.close();
        return path;
    }

    bool replaceFile(const QString &source, const QString &target, QString *message)
    {
        const QFileInfo targetInfo(target);
        const QString backupPattern = QDir(targetInfo.absolutePath()).filePath(
            targetInfo.fileName() + ".pdfapp-backup-XXXXXX");
        QTemporaryDir backupDirectory(backupPattern);
        if (!backupDirectory.isValid()) {
            *message = "Could not create a temporary backup folder beside the PDF.";
            return false;
        }
        const QString backup = backupDirectory.filePath(targetInfo.fileName());

        QFile original(target);
        if (!original.rename(backup)) {
            *message = "Could not move the original PDF for replacement ("
                       + original.errorString() + "). Is it open in another program?";
            return false;
        }

        if (!QFile::rename(source, target)) {
            if (QFile::rename(backup, target)) {
                *message = "Could not install the saved PDF; the original was restored.";
            } else {
                backupDirectory.setAutoRemove(false);
                *message = "Could not install the saved PDF or restore the original. "
                           "Your original PDF is still available at: " + backup;
            }
            return false;
        }

        if (!QFile::remove(backup)) {
            backupDirectory.setAutoRemove(false);
            *message = "Saved, but the backup could not be removed: " + backup;
        }
        return true;
    }

    QByteArray encodeImageForPdf(const QImage &source)
    {
        QImage image = source;
        if (qMax(image.width(), image.height()) > kMaxImageSide)
            image = image.scaled(kMaxImageSide, kMaxImageSide, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);

        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (image.hasAlphaChannel())
            image.save(&buffer, "PNG");
        else
            image.save(&buffer, "JPEG", 92);
        return bytes;
    }

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupViewer();
    setupThumbnailPane();
    setupSearchPane();
    setupBookmarkPane();
    setupMenus();
    setupToolbar();
    setupImageToolbar();
    setupTextToolbar();
    setupDrawToolbar();
    updateActions();

    setAcceptDrops(true);
    setWindowTitle("PDF Reader");
    resize(900, 700);

    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    restoreState(settings.value("window/state").toByteArray());
}

void MainWindow::setupViewer()
{
    m_document = new QPdfDocument(this);
    m_pageRenderDocument = new QPdfDocument(this);

    m_view = new PageView(this);
    m_view->setDocument(m_document);
    m_view->setZoomMode(PageView::ZoomMode::FitToWidth);
    setCentralWidget(m_view);

    connect(m_view, &PageView::textPlaced, this, &MainWindow::placeText);
    connect(m_view, &PageView::textToolFinished, this, [this]() {
        m_addTextAction->setChecked(false);
    });
    connect(m_view, &PageView::strokesPlaced, this, &MainWindow::placeStrokes);
    connect(m_view, &PageView::eraseRequested, this, &MainWindow::eraseInk);
    connect(m_view, &PageView::pendingStrokesChanged, this, &MainWindow::updateActions);
    connect(m_view, &PageView::imagePlaced, this, &MainWindow::placeImage);
    connect(m_view, &PageView::textSelected, this, [this]() {
        statusBar()->showMessage("Text selected: press Ctrl+C or right-click to copy it.", 5000);
    });
}

void MainWindow::setupThumbnailPane()
{
    m_thumbnails = new ThumbnailPane(this);
    m_thumbnails->setDocument(m_document);

    m_pagesDock = new QDockWidget("Pages", this);
    m_pagesDock->setObjectName("pagesDock");
    m_pagesDock->setWidget(m_thumbnails);
    m_pagesDock->setFeatures(QDockWidget::DockWidgetClosable |
                             QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, m_pagesDock);

    connect(m_thumbnails, &ThumbnailPane::pageRequested, this, &MainWindow::goToPage);
    connect(m_view, &PageView::currentPageChanged,
            m_thumbnails, &ThumbnailPane::setCurrentPage);

    connect(m_thumbnails, &ThumbnailPane::pageMoveRequested, this,
            &MainWindow::movePage, Qt::QueuedConnection);
}

void MainWindow::setupSearchPane()
{
    m_search = new SearchPane(this);
    m_search->setDocument(m_document);
    m_view->setSearchModel(m_search->searchModel());

    m_searchDock = new QDockWidget("Search", this);
    m_searchDock->setObjectName("searchDock");
    m_searchDock->setWidget(m_search);
    m_searchDock->setFeatures(QDockWidget::DockWidgetClosable |
                              QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, m_searchDock);
    tabifyDockWidget(m_pagesDock, m_searchDock);
    m_pagesDock->raise();

    connect(m_search, &SearchPane::resultSelected, this,
            [this](int index, int page, QPointF location) {
                m_view->jumpToPage(page, location);
                m_view->setCurrentSearchResultIndex(index);
            });
}

void MainWindow::setupBookmarkPane()
{
    m_bookmarks = new BookmarkPane(this);
    m_bookmarks->setDocument(m_document);

    m_bookmarksDock = new QDockWidget("Bookmarks", this);
    m_bookmarksDock->setObjectName("bookmarksDock");
    m_bookmarksDock->setWidget(m_bookmarks);
    m_bookmarksDock->setFeatures(QDockWidget::DockWidgetClosable |
                                 QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, m_bookmarksDock);
    tabifyDockWidget(m_pagesDock, m_bookmarksDock);
    m_pagesDock->raise();

    connect(m_bookmarks, &BookmarkPane::bookmarkSelected, this,
            [this](int page, QPointF location) {
                m_view->jumpToPage(page, location);
            });
}

void MainWindow::setupMenus()
{
    QMenu *fileMenu = menuBar()->addMenu("&File");
    QAction *openAction = fileMenu->addAction("&Open...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

    m_recentMenu = fileMenu->addMenu("Open &Recent");
    updateRecentMenu();

    fileMenu->addSeparator();

    m_saveAction = fileMenu->addAction("&Save");
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::save);

    m_saveAsAction = fileMenu->addAction("Save &As...");
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::saveAs);

    m_extractAction = fileMenu->addAction("&Extract pages...");
    connect(m_extractAction, &QAction::triggered, this, &MainWindow::extractPages);

    m_insertAction = fileMenu->addAction("&Insert pages from PDF...");
    connect(m_insertAction, &QAction::triggered, this, &MainWindow::insertPages);

    fileMenu->addSeparator();

    m_setPasswordAction = fileMenu->addAction("Set &password...");
    connect(m_setPasswordAction, &QAction::triggered, this, &MainWindow::setDocumentPassword);

    m_removePasswordAction = fileMenu->addAction("Re&move password");
    connect(m_removePasswordAction, &QAction::triggered, this, &MainWindow::removeDocumentPassword);

    QMenu *editMenu = menuBar()->addMenu("&Edit");
    m_undoAction = editMenu->addAction("&Undo");
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undo);

    m_redoAction = editMenu->addAction("&Redo");
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redo);

    editMenu->addSeparator();

    m_highlightAction = editMenu->addAction("&Highlight text...");
    m_highlightAction->setShortcut(Qt::CTRL | Qt::Key_H);
    connect(m_highlightAction, &QAction::triggered, this, &MainWindow::highlightText);

    m_addTextAction = editMenu->addAction("Add &text");
    m_addTextAction->setCheckable(true);
    m_addTextAction->setShortcut(Qt::CTRL | Qt::Key_T);
        connect(m_addTextAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_view->commitPendingEdits();
        m_view->setTextToolActive(on);
        if (on) {
            statusBar()->showMessage(
                "Drag a box on the page, type your text, then click elsewhere to place it "
                "(Ctrl+Enter also places it, Esc cancels).", 10000);
        }
    });

    m_drawAction = editMenu->addAction("&Draw");
    m_drawAction->setCheckable(true);
    m_drawAction->setShortcut(Qt::CTRL | Qt::Key_D);
    connect(m_drawAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_view->commitPendingEdits();
        m_view->setDrawToolActive(on);
        if (on) {
            statusBar()->showMessage(
                "Draw strokes or erase existing Ink strokes. The eraser does not affect "
                "highlights or images. Ctrl+Z removes your last pending stroke. "
                "Turn Draw off to add everything to the document.", 10000);
        }
    });

    auto *toolGroup = new QActionGroup(this);
    toolGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    toolGroup->addAction(m_addTextAction);
    toolGroup->addAction(m_drawAction);

    m_addImageAction = editMenu->addAction("Add &image...");
    m_addImageAction->setShortcut(Qt::CTRL | Qt::Key_I);
    connect(m_addImageAction, &QAction::triggered, this, &MainWindow::addImage);

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

    QMenu *helpMenu = menuBar()->addMenu("&Help");
    connect(helpMenu->addAction("&About"), &QAction::triggered, this, &MainWindow::showAbout);
    connect(helpMenu->addAction("About &Qt"), &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::setupToolbar()
{
    QToolBar *toolbar = addToolBar("Navigation");
    toolbar->setObjectName("navigationToolbar");
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
    m_rotateLeftAction->setShortcut(Qt::CTRL | Qt::Key_L);
    m_rotateRightAction = toolbar->addAction("Rotate right");
    m_rotateRightAction->setShortcut(Qt::CTRL | Qt::Key_R);
    connect(m_rotateLeftAction, &QAction::triggered, this, [this]() { rotateSelectedPages(-90); });
    connect(m_rotateRightAction, &QAction::triggered, this, [this]() { rotateSelectedPages(90); });

    m_deletePageAction = toolbar->addAction("Delete pages");
    connect(m_deletePageAction, &QAction::triggered, this, &MainWindow::deleteSelectedPages);

    // Wiring
    connect(prevAction, &QAction::triggered, this, [this]() {
        goToPage(m_view->currentPage() - 1);
    });
    connect(nextAction, &QAction::triggered, this, [this]() {
        goToPage(m_view->currentPage() + 1);
    });
    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int value) {
        goToPage(value - 1);
    });
    connect(m_view, &PageView::currentPageChanged, this, [this](int page) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setValue(page + 1);
    });
    connect(m_document, &QPdfDocument::pageCountChanged, this, [this](int count) {
        QSignalBlocker blocker(m_pageBox);
        m_pageBox->setRange(count > 0 ? 1 : 0, count);
        m_pageCountLabel->setText(QString(" / %1 ").arg(count));
    });

    connect(zoomInAction, &QAction::triggered, this, [this]() {
        m_view->setZoomFactor(m_view->zoomFactor() * 1.25);
    });
    connect(zoomOutAction, &QAction::triggered, this, [this]() {
        m_view->setZoomFactor(m_view->zoomFactor() / 1.25);
    });
    connect(fitWidthAction, &QAction::triggered, this, [this]() {
        m_view->setZoomMode(PageView::ZoomMode::FitToWidth);
    });
    connect(fitPageAction, &QAction::triggered, this, [this]() {
        m_view->setZoomMode(PageView::ZoomMode::FitInView);
    });
}

void MainWindow::setupImageToolbar()
{
    addToolBarBreak();
    QToolBar *toolbar = addToolBar("Image");
    toolbar->setObjectName("imageToolbar");
    toolbar->setMovable(false);

    toolbar->addAction(m_addImageAction);
}

void MainWindow::setupTextToolbar()
{
    addToolBarBreak();
    QToolBar *toolbar = addToolBar("Text");
    toolbar->setObjectName("textToolbar");
    toolbar->setMovable(false);

    toolbar->addAction(m_addTextAction);

    auto *fontBox = new QFontComboBox(this);
    fontBox->setEditable(false);
    fontBox->setFontFilters(QFontComboBox::ScalableFonts);
    fontBox->setCurrentFont(QFont("Arial"));
    fontBox->setMinimumWidth(190);
    fontBox->setFocusPolicy(Qt::NoFocus);
    toolbar->addWidget(fontBox);

    QAction *boldAction = toolbar->addAction("B");
    boldAction->setCheckable(true);
    boldAction->setToolTip("Bold");
    QFont boldFont = boldAction->font();
    boldFont.setBold(true);
    boldAction->setFont(boldFont);

    QAction *italicAction = toolbar->addAction("I");
    italicAction->setCheckable(true);
    italicAction->setToolTip("Italic");
    QFont italicFont = italicAction->font();
    italicFont.setItalic(true);
    italicAction->setFont(italicFont);

    auto *size = new QSpinBox(this);
    size->setRange(6, 200);
    size->setValue(12);
    size->setSuffix(" pt");
    size->setFocusPolicy(Qt::NoFocus);
    toolbar->addWidget(size);

    auto *colorButton = new ColorMenuButton(QColor(0, 0, 0), this);
    toolbar->addWidget(colorButton);

    auto applyStyle = [this, fontBox, boldAction, italicAction, size, colorButton]() {
        PageView::TextStyle style;
        style.family = fontBox->currentFont().family();
        style.bold = boldAction->isChecked();
        style.italic = italicAction->isChecked();
        style.sizePoints = size->value();
        style.color = colorButton->color();
        m_view->setTextStyle(style);
    };
    connect(fontBox, &QFontComboBox::currentFontChanged, this, applyStyle);
    connect(boldAction, &QAction::toggled, this, applyStyle);
    connect(italicAction, &QAction::toggled, this, applyStyle);
    connect(size, &QSpinBox::valueChanged, this, applyStyle);
    connect(colorButton, &ColorMenuButton::colorChanged, this, applyStyle);

    toolbar->addSeparator();
    applyStyle();
}

void MainWindow::setupDrawToolbar()
{
    addToolBarBreak();
    QToolBar *toolbar = addToolBar("Draw");
    toolbar->setObjectName("drawToolbar");
    toolbar->setMovable(false);

    toolbar->addAction(m_drawAction);

    auto *mode = new QComboBox(this);
    mode->addItem("Pen");
    mode->addItem("Highlighter");
    mode->addItem("White-out");
    mode->addItem("Eraser");
    mode->setFocusPolicy(Qt::NoFocus);
    toolbar->addWidget(mode);

    toolbar->addWidget(new QLabel(" Size ", this));
    auto *size = new QSpinBox(this);
    size->setRange(1, 60);
    size->setValue(2);
    size->setSuffix(" pt");
    size->setFocusPolicy(Qt::NoFocus);
    toolbar->addWidget(size);

    toolbar->addWidget(new QLabel(" Opacity ", this));
    auto *opacity = new QSpinBox(this);
    opacity->setRange(5, 100);
    opacity->setValue(100);
    opacity->setSuffix(" %");
    opacity->setFocusPolicy(Qt::NoFocus);
    toolbar->addWidget(opacity);

    auto *colorButton = new ColorMenuButton(QColor(0, 0, 0), this);
    toolbar->addWidget(colorButton);

    auto applyStyle = [this, size, opacity, colorButton]() {
        PageView::DrawStyle style;
        style.color = colorButton->color();
        style.widthPoints = size->value();
        style.opacity = opacity->value() / 100.0;
        m_view->setDrawStyle(style);
    };
    connect(size, &QSpinBox::valueChanged, this, applyStyle);
    connect(opacity, &QSpinBox::valueChanged, this, applyStyle);
    connect(colorButton, &ColorMenuButton::colorChanged, this, applyStyle);

    connect(mode, &QComboBox::currentIndexChanged, this,
            [this, size, opacity, colorButton, applyStyle](int index) {
                if (index == 3)
                    m_view->commitPendingEdits();
                if (index == 0) {
                    size->setValue(2);
                    opacity->setValue(100);
                    colorButton->setColor(QColor(0, 0, 0));
                } else if (index == 1) {
                    size->setValue(14);
                    opacity->setValue(35);
                    colorButton->setColor(QColor(255, 220, 0));
                } else if (index == 2) {
                    size->setValue(16);
                    opacity->setValue(100);
                    colorButton->setColor(QColor(255, 255, 255));
                } else {
                    size->setValue(16);
                }
                opacity->setEnabled(index != 3);
                colorButton->setEnabled(index != 3);
                m_view->setEraserActive(index == 3);
                applyStyle();
            });
    applyStyle();
}

// opening files

void MainWindow::openFile()
{
    if (!confirmDiscard())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, "Open PDF", QString(), "PDF files (*.pdf)");
    if (path.isEmpty())
        return;

    loadDocument(path);
}

void MainWindow::openPath(const QString &path)
{
    if (!confirmDiscard())
        return;
    loadDocument(path);
}

void MainWindow::loadDocument(const QString &path)
{
    const QString previousState = m_history.current();
    const int previousPage = m_view->currentPage();
    auto restorePreviousDocument = [this, &previousState, previousPage]() {
        if (previousState.isEmpty())
            return;
        if (!showFile(previousState, previousPage, false)) {
            QMessageBox::warning(
                this, "Open failed",
                "The new PDF could not be opened, and the previous PDF could not be "
                "restored. Try reopening the previous file.");
        }
    };

    clearPageRenderDocument();
    m_document->close();
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
            restorePreviousDocument();
            updateTitle();
            return;
        }

        m_document->setPassword(password);
        usedPassword = password;
        error = m_document->load(path);
    }

    if (error != QPdfDocument::Error::None) {
        restorePreviousDocument();
        updateTitle();
        QMessageBox::warning(this, "Error", "Could not open that PDF.");
        return;
    }

    m_editor.reset();
    removeSnapshots(m_history.states());
    m_history.clear();
    m_currentPath = path;
    m_password = usedPassword;
    m_passwordChange = PasswordChange::None;
    m_newPassword.clear();
    m_displayName = QFileInfo(path).fileName();
    addRecentFile(path);

    openWorkingCopy(path, 0);
    if (!m_editor)
        statusBar()->showMessage("Editing is not available for this file.", 5000);

    updateActions();
    updateTitle();
}

void MainWindow::goToPage(int page)
{
    if (page < 0 || page >= m_document->pageCount())
        return;
    m_view->jumpToPage(page);
}

bool MainWindow::showFile(const QString &path, int page, bool reportErrors)
{
    clearPageRenderDocument();
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

void MainWindow::clearPageRenderDocument()
{
    if (m_view)
        m_view->setPageRenderDocument(nullptr);
    if (m_thumbnails)
        m_thumbnails->setPageRenderDocument(nullptr);
    if (m_pageRenderDocument)
        m_pageRenderDocument->close();
}

// recent files, about, drops

void MainWindow::addRecentFile(const QString &path)
{
    QSettings settings;
    QStringList recent = settings.value("recentFiles").toStringList();

    const QString full = QFileInfo(path).absoluteFilePath();
    recent.removeAll(full);
    recent.prepend(full);
    while (recent.size() > kMaxRecentFiles)
        recent.removeLast();
    settings.setValue("recentFiles", recent);

    QTimer::singleShot(0, this, &MainWindow::updateRecentMenu);
}

void MainWindow::updateRecentMenu()
{
    m_recentMenu->clear();

    QSettings settings;
    QStringList recent = settings.value("recentFiles").toStringList();
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [](const QString &p) { return !QFileInfo::exists(p); }),
                 recent.end());
    settings.setValue("recentFiles", recent);

    m_recentMenu->setEnabled(!recent.isEmpty());
    if (recent.isEmpty())
        return;

    for (int i = 0; i < recent.size(); ++i) {
        const QString path = recent.at(i);
        QAction *action = m_recentMenu->addAction(
            QString("&%1  %2").arg(i + 1).arg(QFileInfo(path).fileName()));
        action->setStatusTip(path);
        connect(action, &QAction::triggered, this, [this, path]() { openPath(path); });
    }

    m_recentMenu->addSeparator();
    QAction *clear = m_recentMenu->addAction("&Clear list");
    connect(clear, &QAction::triggered, this, [this]() {
        QSettings().remove("recentFiles");
        QTimer::singleShot(0, this, &MainWindow::updateRecentMenu);
    });
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, "About PDF Reader",
        "<h3>Offline PDF Reader/Editor</h3>"
        "<p>A desktop PDF reader and page editor built with Qt 6 and MuPDF.</p>"
        "<p>Licensed under the GNU Affero General Public License v3.0.<br>"
        "Source code: <a href=\"https://github.com/genecc04/offline-pdf-reader-editor\">"
        "github.com/genecc04/offline-pdf-reader-editor</a></p>"
        "<p>Uses MuPDF (Artifex Software, Inc.) and Qt.</p>");
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!droppedPdf(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QString path = droppedPdf(event->mimeData());
    if (path.isEmpty())
        return;

    event->acceptProposedAction();
    QTimer::singleShot(0, this, [this, path]() { openPath(path); });
}

// saving, editing

bool MainWindow::saveAs()
{
    m_view->commitPendingEdits();
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
    if (!saver.open(m_history.current(), m_password)
        || !saver.saveAs(dest, true, saveEncryption(), m_newPassword)) {
        QMessageBox::warning(this, "Save failed", saver.lastError());
        return false;
    }

    m_history.markSaved();
    m_passwordChange = PasswordChange::None;
    m_newPassword.clear();
    updateActions();
    updateTitle();
    statusBar()->showMessage("Saved " + QFileInfo(dest).fileName(), 4000);
    return true;
}

bool MainWindow::save()
{
    m_view->commitPendingEdits();
    if (!m_editor || !isDocumentModified())
        return true;

    const QString target = m_currentPath;
    if (!QFileInfo(target).isWritable()) {
        QMessageBox::warning(this, "Save",
                             "This file is read-only. Use Save As to save a copy.");
        return false;
    }

    QString tempError;
    const QString tmp = createSiblingTempPath(target, "tmp", &tempError);
    if (tmp.isEmpty()) {
        QMessageBox::warning(this, "Save failed", tempError);
        return false;
    }
    const int page = m_view->currentPage();

    {
        PdfEditor saver;
        if (!saver.open(m_history.current(), m_password)
            || !saver.saveAs(tmp, true, saveEncryption(), m_newPassword)) {
            QFile::remove(tmp);
            QMessageBox::warning(this, "Save failed", saver.lastError());
            return false;
        }
    }

    m_editor.reset();
    m_document->close();

    QString replacementMessage;
    if (!replaceFile(tmp, target, &replacementMessage)) {
        QFile::remove(tmp);
        showFile(m_history.current(), page, false);
        reopenEditor(m_history.current());
        updateActions();
        QMessageBox::warning(this, "Save failed", replacementMessage);
        return false;
    }

    if (m_passwordChange == PasswordChange::Set)
        m_password = m_newPassword;
    else if (m_passwordChange == PasswordChange::Remove)
        m_password.clear();
    m_passwordChange = PasswordChange::None;
    m_newPassword.clear();

    removeSnapshots(m_history.states());
    openWorkingCopy(target, page);
    updateActions();
    updateTitle();
    statusBar()->showMessage(
        replacementMessage.isEmpty() ? "Saved " + m_displayName : replacementMessage,
        replacementMessage.isEmpty() ? 4000 : 10000);
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

bool MainWindow::refreshAfterPageEdit(const QList<int> &changedPages, int page)
{
    if (!m_tempDir.isValid()) {
        QMessageBox::warning(this, "Edit failed", "Could not create a temporary folder.");
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    QList<int> pages;
    for (int changedPage : changedPages) {
        if (changedPage >= 0 && changedPage < m_document->pageCount()
            && !pages.contains(changedPage))
            pages.append(changedPage);
    }
    if (pages.isEmpty())
        return refreshAfterEdit(page);

    const QString newPath = m_tempDir.filePath(
        QString("state-%1.pdf").arg(++m_editCount));
    if (!m_editor->saveAs(newPath)) {
        QMessageBox::warning(this, "Edit failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }

    m_pageRenderDocument->close();
    m_pageRenderDocument->setPassword(m_password);
    if (m_pageRenderDocument->load(newPath) != QPdfDocument::Error::None) {
        if (!showFile(newPath, page)) {
            QFile::remove(newPath);
            showFile(m_history.current(), page, false);
            reopenEditor(m_history.current());
            updateActions();
            return false;
        }
    } else {
        m_view->setPageRenderDocument(m_pageRenderDocument, pages);
        m_thumbnails->setPageRenderDocument(m_pageRenderDocument, pages);
    }

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
        pages << m_view->currentPage();
    return pages;
}

void MainWindow::rotateSelectedPages(int degrees)
{
    if (!m_editor)
        return;

    const QList<int> pages = targetPages();
    const int viewPage = m_view->currentPage();

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

    const QList<int> pages = targetPages();    // ascending order
    const int pageCount = m_document->pageCount();
    if (pages.size() >= pageCount) {
        QMessageBox::information(this, "Delete pages",
                                 "A PDF must keep at least one page.");
        return;
    }

    for (int i = pages.size() - 1; i >= 0; --i) {
        if (!m_editor->deletePage(pages.at(i))) {
            QMessageBox::warning(this, "Delete failed", m_editor->lastError());
            reopenEditor(m_history.current());   // throw away the half-finished edit
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
    m_view->commitPendingEdits();
    if (!m_editor)
        return;

    const int pageCount = m_document->pageCount();
    const int current = m_view->currentPage();

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
        || !extractor.saveAs(dest, true, saveEncryption(), m_newPassword)) {
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
    const int current = m_view->currentPage();

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

void MainWindow::highlightText()
{
    if (!m_editor)
        return;

    bool ok = false;
    const QString text = QInputDialog::getText(
        this, "Highlight text", "Highlight every occurrence of:",
        QLineEdit::Normal, QString(), &ok);
    if (!ok || text.trimmed().isEmpty())
        return;

    const int page = m_view->currentPage();
    QList<int> changedPages;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool done = m_editor->highlightText(text.trimmed(), &changedPages);
    QApplication::restoreOverrideCursor();

    if (!done) {
        QMessageBox::warning(this, "Highlight failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (changedPages.isEmpty()) {
        statusBar()->showMessage("No matches found.", 4000);
        return;
    }

    if (refreshAfterPageEdit(changedPages, page))
        statusBar()->showMessage(
            QString("Highlighted text on %1 page(s)").arg(changedPages.size()), 4000);
}

void MainWindow::placeText(int page, const QRectF &box, const QString &text,
                           const PageView::TextStyle &style)
{
    if (!m_editor)
        return;

    const QByteArray png = renderTextToPng(text, style, box.size());
    if (png.isEmpty()) {
        QMessageBox::warning(this, "Add text failed", "Could not draw the text.");
        return;
    }

    if (!m_editor->addImageStamp(page, box, png)) {
        QMessageBox::warning(this, "Add text failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    refreshAfterPageEdit({ page }, page);
}

void MainWindow::addImage()
{
    if (!m_editor)
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, "Choose an image", QString(),
        "Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff);;All files (*)");
    if (path.isEmpty())
        return;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize sourceSize = reader.size();
    if (sourceSize.isValid() && qMax(sourceSize.width(), sourceSize.height()) > kMaxImageSide) {
        QSize scaledSize = sourceSize;
        scaledSize.scale(kMaxImageSide, kMaxImageSide, Qt::KeepAspectRatio);
        reader.setScaledSize(scaledSize);
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        QMessageBox::warning(this, "Add image",
                             "Could not read that image: " + reader.errorString());
        return;
    }

    m_addTextAction->setChecked(false);
    m_drawAction->setChecked(false);

    m_view->beginImagePlacement(image);
    statusBar()->showMessage(
        "Click on the page where the image goes. Drag to move it, drag a corner to resize, "
        "Enter or a click elsewhere places it, Esc cancels.", 12000);
}

void MainWindow::placeImage(int page, const QRectF &box, const QImage &image)
{
    if (!m_editor)
        return;

    const QByteArray data = encodeImageForPdf(image);
    if (data.isEmpty()) {
        QMessageBox::warning(this, "Add image failed", "Could not prepare the image.");
        return;
    }

    if (!m_editor->addImageStamp(page, box, data)) {
        QMessageBox::warning(this, "Add image failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    refreshAfterPageEdit({ page }, page);
}

void MainWindow::placeStrokes(const QVector<PageView::Stroke> &strokes)
{
    if (!m_editor || strokes.isEmpty())
        return;

    for (const PageView::Stroke &stroke : strokes) {
        if (!m_editor->addInkStroke(stroke.page, stroke.points,
                                    stroke.style.color.redF(), stroke.style.color.greenF(),
                                    stroke.style.color.blueF(),
                                    stroke.style.widthPoints, stroke.style.opacity)) {
            QMessageBox::warning(this, "Draw failed", m_editor->lastError());
            reopenEditor(m_history.current());
            updateActions();
            return;
        }
    }

    QList<int> changedPages;
    for (const PageView::Stroke &stroke : strokes) {
        if (!changedPages.contains(stroke.page))
            changedPages.append(stroke.page);
    }
    if (refreshAfterPageEdit(changedPages, strokes.last().page))
        statusBar()->showMessage(QString("Added %1 stroke(s)").arg(strokes.size()), 3000);
}

void MainWindow::eraseInk(int page, const QVector<QPointF> &path, double radius)
{
    if (!m_editor)
        return;

    int removed = 0;
    if (!m_editor->eraseInk(page, path, radius, &removed)) {
        QMessageBox::warning(this, "Erase failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (removed == 0) {
        statusBar()->showMessage("No drawing strokes under the eraser.", 2500);
        return;
    }

    if (refreshAfterPageEdit({ page }, page))
        statusBar()->showMessage(
            QString("Erased %1 drawing stroke(s)").arg(removed), 3000);
}

void MainWindow::undo()
{
    if (m_view->undoLastStroke())
        return;
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

    const int page = m_view->currentPage();
    if (!showFile(path, page))
        return;

    if (!reopenEditor(path))
        QMessageBox::warning(this, "Editing", "Editing is no longer available for this file.");

    updateActions();
    updateTitle();
}

// document state

bool MainWindow::confirmDiscard()
{
    m_view->commitPendingEdits();
    if (!isDocumentModified())
        return true;

    const auto answer = QMessageBox::question(
        this, "Unsaved changes",
        "You have unsaved changes. Save them first?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Save)
        return save();
    return answer == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscard()) {
        event->ignore();
        return;
    }

    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    event->accept();
}

void MainWindow::updateTitle()
{
    if (m_displayName.isEmpty())
        setWindowTitle("PDF Reader");
    else
        setWindowTitle(m_displayName + (isDocumentModified() ? " *" : "") + " - PDF Reader");
}

void MainWindow::updateActions()
{
    const bool editable = (m_editor != nullptr);

    m_saveAction->setEnabled(editable && isDocumentModified());
    m_setPasswordAction->setEnabled(editable);
    m_removePasswordAction->setEnabled(editable && isPasswordProtected());
    m_extractAction->setEnabled(editable);
    m_insertAction->setEnabled(editable);
    m_highlightAction->setEnabled(editable);
    m_addTextAction->setEnabled(editable);
    if (!editable && m_addTextAction->isChecked())
        m_addTextAction->setChecked(false);
    m_rotateLeftAction->setEnabled(editable);
    m_rotateRightAction->setEnabled(editable);
    m_deletePageAction->setEnabled(editable);
    m_thumbnails->setReorderEnabled(editable);

    m_undoAction->setEnabled(editable && (m_history.canUndo() || m_view->hasPendingStrokes()));
    m_redoAction->setEnabled(editable && m_history.canRedo());
    m_addImageAction->setEnabled(editable);

    m_drawAction->setEnabled(editable);
    if (!editable && m_drawAction->isChecked())
        m_drawAction->setChecked(false);
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

bool MainWindow::isDocumentModified() const
{
    return m_history.isModified() || m_passwordChange != PasswordChange::None;
}

bool MainWindow::isPasswordProtected() const
{
    return m_passwordChange == PasswordChange::Set
           || (m_passwordChange == PasswordChange::None && !m_password.isEmpty());
}

PdfEditor::Encryption MainWindow::saveEncryption() const
{
    switch (m_passwordChange) {
    case PasswordChange::Set:    return PdfEditor::Encryption::Aes256;
    case PasswordChange::Remove: return PdfEditor::Encryption::Remove;
    default:                     return PdfEditor::Encryption::Keep;
    }
}

void MainWindow::setDocumentPassword()
{
    if (!m_editor)
        return;

    bool ok = false;
    const QString first = QInputDialog::getText(
        this, "Set password",
        "Enter a password. Anyone opening the saved file will need it.\n"
        "If you forget it, the file cannot be recovered.",
        QLineEdit::Password, QString(), &ok);
    if (!ok)
        return;
    if (first.isEmpty()) {
        QMessageBox::warning(this, "Set password", "The password cannot be empty.");
        return;
    }
    if (first.toUtf8().size() > 127) {
        QMessageBox::warning(this, "Set password",
                             "That password is too long (127 characters at most).");
        return;
    }

    const QString second = QInputDialog::getText(
        this, "Set password", "Enter the password again to confirm:",
        QLineEdit::Password, QString(), &ok);
    if (!ok)
        return;
    if (second != first) {
        QMessageBox::warning(this, "Set password", "The two passwords don't match.");
        return;
    }

    m_passwordChange = PasswordChange::Set;
    m_newPassword = first;
    updateActions();
    updateTitle();
    statusBar()->showMessage(
        "The password will be applied when you save (File > Save or Save As).", 8000);
}

void MainWindow::removeDocumentPassword()
{
    if (!m_editor)
        return;

    const auto answer = QMessageBox::question(
        this, "Remove password",
        "Remove the password? Anyone will be able to open the saved file without one.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_passwordChange = m_password.isEmpty() ? PasswordChange::None : PasswordChange::Remove;
    m_newPassword.clear();
    updateActions();
    updateTitle();
    statusBar()->showMessage(m_passwordChange == PasswordChange::Remove
        ? "The password will be removed when you save (File > Save or Save As)."
        : "No password will be set.", 8000);
}

bool MainWindow::openWorkingCopy(const QString &source, int page)
{
    QString working = source;
    if (m_tempDir.isValid()) {
        const QString copy = m_tempDir.filePath(QString("state-%1.pdf").arg(++m_editCount));
        if (QFile::copy(source, copy))
            working = copy;
    }

    m_history.reset(working);
    const bool shown = showFile(working, page);
    reopenEditor(working);
    return shown;
}