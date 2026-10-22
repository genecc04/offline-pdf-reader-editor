#include "MainWindow.h"
#include "ThumbnailPane.h"
#include "SearchPane.h"
#include "BookmarkPane.h"
#include "PdfEditor.h"
#include "PageRangeDialog.h"
#include "ColorMenuButton.h"

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
#include <QKeySequence>
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

    QString annotationFontName(const PageView::TextStyle &style)
    {
        const QString family = style.family.toLower();
        if (family.contains("times") || family.contains("serif"))
            return QStringLiteral("TiRo");
        if (family.contains("courier") || family.contains("mono"))
            return QStringLiteral("Cour");
        return QStringLiteral("Helv");
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
    connect(m_view, &PageView::textAnnotationEditRequested, m_view,
            &PageView::editTextAnnotationInline);
    connect(m_view, &PageView::textAnnotationEdited,
            this, &MainWindow::updateTextAnnotation);
    connect(m_view, &PageView::textAnnotationDeleteRequested,
            this, &MainWindow::deleteTextAnnotation);
    connect(m_view, &PageView::textAnnotationBoundsChanged,
            this, &MainWindow::updateTextAnnotationBounds);
    connect(m_view, &PageView::textAnnotationSelectionChanged, this, [this](bool selected) {
        if (m_deleteTextAction)
            m_deleteTextAction->setEnabled(selected && m_editor != nullptr);
    });
    connect(m_view, &PageView::textToolFinished, this, [this]() {
        m_addTextAction->setChecked(false);
    });
    connect(m_view, &PageView::strokesPlaced, this, &MainWindow::placeStrokes);
    connect(m_view, &PageView::eraseRequested, this, &MainWindow::eraseInk);
    connect(m_view, &PageView::pendingStrokesChanged, this, [this]() {
        updateActions();
        updateTitle();
    });
    connect(m_view, &PageView::pendingChangesChanged, this, [this]() {
        updateActions();
        updateTitle();
    });
    connect(m_view, &PageView::imagePlaced, this, &MainWindow::placeImage);
    connect(m_view, &PageView::imagePlacementStateChanged, this,
            [this](bool active, bool positioned) {
        m_cancelImageAction->setEnabled(active);
        m_cropImageAction->setEnabled(positioned);
        if (!positioned) {
            QSignalBlocker blocker(m_cropImageAction);
            m_cropImageAction->setChecked(false);
        }
    });
    connect(m_view, &PageView::imageCropModeChanged, this, [this](bool active) {
        {
            QSignalBlocker blocker(m_cropImageAction);
            m_cropImageAction->setChecked(active);
        }
        if (active) {
            statusBar()->showMessage(
                "Crop mode: drag a corner inward to crop. Select Crop again to finish cropping.",
                12000);
        } else if (m_cancelImageAction->isEnabled()) {
            statusBar()->showMessage(
                "Image placement: drag to move or resize. Select Crop to crop; Enter places; "
                "Esc cancels.",
                12000);
        } else {
            statusBar()->clearMessage();
        }
    });
    m_view->setFormFieldHandler(
        [this](int page, const QPointF &point, PageView::FormField *field) {
            return fillFormField(page, point, field);
        });
    m_view->setFormFieldValueHandler(
        [this](int page, int widgetIndex, const QString &value) {
            return commitFormField(page, widgetIndex, value);
        });
    m_view->setTextAnnotationHandler(
        [this](int page, const QPointF &point, int *annotationId,
               QRectF *bounds, QString *text, PageView::TextStyle *style) {
            QString fontName;
            double fontSize = 12.0;
            QColor color;
            if (!m_editor
                || !m_editor->textAnnotationAt(page, point, annotationId, bounds, text,
                                               &fontName, &fontSize, &color))
                return false;

            style->family = fontName == QLatin1String("TiRo")
                ? QStringLiteral("Times")
                : fontName == QLatin1String("Cour")
                    ? QStringLiteral("Courier")
                    : QStringLiteral("Helvetica");
            style->sizePoints = fontSize;
            style->color = color;
            return true;
        });
    connect(m_view, &PageView::openRequested, this, &MainWindow::openFile);
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
                "Drag a box on the page and type. Click elsewhere to place it. "
                "Double-click added text to edit; right-click it to edit or delete.",
                10000);
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
        restorePendingPage();
    });
    connect(m_document, &QPdfDocument::statusChanged, this, [this]() {
        restorePendingPage();
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
    toolbar->addSeparator();

    m_cropImageAction = toolbar->addAction("Crop");
    m_cropImageAction->setCheckable(true);
    m_cropImageAction->setEnabled(false);
    connect(m_cropImageAction, &QAction::toggled,
            m_view, &PageView::setImageCropMode);

    m_cancelImageAction = toolbar->addAction("Cancel");
    m_cancelImageAction->setEnabled(false);
    m_cancelImageAction->setShortcut(QKeySequence(Qt::Key_Escape));
    m_cancelImageAction->setShortcutContext(Qt::WindowShortcut);
    addAction(m_cancelImageAction);
    connect(m_cancelImageAction, &QAction::triggered,
            m_view, &PageView::cancelImagePlacement);
}

void MainWindow::setupTextToolbar()
{
    addToolBarBreak();
    QToolBar *toolbar = addToolBar("Text");
    toolbar->setObjectName("textToolbar");
    toolbar->setMovable(false);

    toolbar->addAction(m_addTextAction);
    m_deleteTextAction = toolbar->addAction("Delete");
    m_deleteTextAction->setEnabled(false);
    m_deleteTextAction->setToolTip("Delete selected text box");
    connect(m_deleteTextAction, &QAction::triggered,
            m_view, &PageView::deleteSelectedTextAnnotation);
    toolbar->addSeparator();

    auto *fontBox = new QComboBox(this);
    fontBox->addItems({ "Helvetica", "Times", "Courier" });
    fontBox->setCurrentText("Helvetica");
    fontBox->setMinimumWidth(130);
    fontBox->setFocusPolicy(Qt::NoFocus);
    toolbar->addWidget(fontBox);

    auto *size = new QSpinBox(this);
    size->setRange(6, 200);
    size->setValue(12);
    size->setSuffix(" pt");
    size->setFocusPolicy(Qt::NoFocus);
    toolbar->addWidget(size);

    auto *colorButton = new ColorMenuButton(QColor(0, 0, 0), this);
    toolbar->addWidget(colorButton);

    auto applyStyle = [this, fontBox, size, colorButton]() {
        PageView::TextStyle style;
        style.family = fontBox->currentText();
        style.sizePoints = size->value();
        style.color = colorButton->color();
        m_view->setTextStyle(style);
    };
    connect(fontBox, &QComboBox::currentTextChanged, this, applyStyle);
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
    m_pendingPage = -1;
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

    QString usedPassword;
    auto authenticatedEditor = std::make_unique<PdfEditor>();
    bool editorAvailable = authenticatedEditor->open(path);
    QString editorOpenError = editorAvailable ? QString() : authenticatedEditor->lastError();
    if (!editorAvailable && editorOpenError == QStringLiteral("Incorrect or missing password")) {
        bool firstAttempt = true;
        while (!editorAvailable) {
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

            auto candidateEditor = std::make_unique<PdfEditor>();
            if (candidateEditor->open(path, password)) {
                authenticatedEditor = std::move(candidateEditor);
                editorAvailable = true;
                usedPassword = password;
            } else {
                editorOpenError = candidateEditor->lastError();
                editorAvailable = false;
                if (editorOpenError != QStringLiteral("Incorrect or missing password"))
                    break;
            }
        }
    }

    clearPageRenderDocument();
    m_document->close();
    m_document->setPassword(usedPassword);
    QPdfDocument::Error error = m_document->load(path);

    if (error != QPdfDocument::Error::None) {
        restorePreviousDocument();
        updateTitle();
        QMessageBox::warning(this, "Error", "Could not open that PDF.");
        return;
    }

    m_view->clearFormFieldOverlays();
    m_editor.reset();
    removeSnapshots(m_history.states());
    m_history.clear();
    m_currentPath = path;
    m_password = usedPassword;
    m_passwordChange = PasswordChange::None;
    m_newPassword.clear();
    m_displayName = QFileInfo(path).fileName();
    addRecentFile(path);

    QString working = path;
    bool workingCopyCreated = false;
    if (m_tempDir.isValid()) {
        const QString copy = m_tempDir.filePath(
            QString("state-%1.pdf").arg(++m_editCount));
        if (QFile::copy(path, copy)) {
            working = copy;
            workingCopyCreated = true;
        }
    }
    m_history.reset(working);
    if (editorAvailable) {
        authenticatedEditor.reset();
        if (workingCopyCreated) {
            if (!showFile(working, 0)) {
                editorAvailable = false;
                editorOpenError = "Could not load the PDF working copy for display.";
                showFile(path, 0, false);
            } else {
                m_editor = std::make_unique<PdfEditor>();
                editorAvailable = m_editor->open(working, m_password);
                if (editorAvailable)
                    refreshFormFieldOverlays();
                else
                    editorOpenError = m_editor->lastError();
            }
        } else {
            editorAvailable = false;
            editorOpenError = "Could not create a working copy of the PDF.";
        }
    }
    if (!editorAvailable) {
        m_editor.reset();
        QMessageBox::warning(
            this, "Editing unavailable",
            "The PDF opened for viewing, but editing could not be started:\n\n"
                + (editorOpenError.isEmpty()
                       ? QStringLiteral("No error details were provided.")
                       : editorOpenError));
    }

    updateActions();
    updateTitle();
}

void MainWindow::goToPage(int page)
{
    const int pageCount = m_document->pageCount();
    if (pageCount <= 0 || page < 0 || page >= pageCount)
        return;
    m_view->jumpToPage(page);
}

bool MainWindow::showFile(const QString &path, int page, bool reportErrors)
{
    m_pendingPage = -1;
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

    const int pageCount = m_document->pageCount();
    if (pageCount <= 0) {
        m_pendingPage = qMax(0, page);
        return true;
    }
    m_pendingPage = -1;
    const int target = qBound(0, page, pageCount - 1);
    goToPage(target);
    m_thumbnails->setCurrentPage(target);
    return true;
}

void MainWindow::restorePendingPage()
{
    if (m_pendingPage < 0
        || m_document->status() != QPdfDocument::Status::Ready
        || m_document->pageCount() <= 0)
        return;

    const int target = qMin(m_pendingPage, m_document->pageCount() - 1);
    m_pendingPage = -1;
    goToPage(target);
    m_thumbnails->setCurrentPage(target);
    if (m_editor)
        refreshFormFieldOverlays();
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
        "<p>Copyright &copy; 2026 genecc04.<br>"
        "This program comes with ABSOLUTELY NO WARRANTY. It is free software, licensed "
        "under the <a href=\"https://www.gnu.org/licenses/agpl-3.0.html\">"
        "GNU Affero General Public License v3.0</a>, and you are welcome to "
        "redistribute it under its conditions.</p>"
        "<p>Source code: <a href=\"https://github.com/genecc04/offline-pdf-reader-editor\">"
        "github.com/genecc04/offline-pdf-reader-editor</a></p>"
        "<p>Uses MuPDF (Artifex Software, Inc.) and Qt. See THIRD_PARTY_NOTICES.md.</p>");
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
    if (reopenEditor(newPath))
        refreshFormFieldOverlays();
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

    if (!m_editor->addTextBox(page, box, text, annotationFontName(style),
                              style.sizePoints, style.color.redF(),
                              style.color.greenF(), style.color.blueF())) {
        QMessageBox::warning(this, "Add text failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    refreshAfterPageEdit({ page }, page);
}

void MainWindow::updateTextAnnotation(int page, int annotationId,
                                      const QString &oldText, const QString &newText)
{
    if (!m_editor)
        return;

    if (newText == oldText)
        return;
    if (newText.trimmed().isEmpty()) {
        QMessageBox::warning(this, "Edit added text",
                             "Text cannot be empty. Use Delete added text to remove it.");
        m_view->editTextAnnotationInline(page, annotationId, oldText);
        return;
    }

    if (!m_editor->updateTextAnnotation(page, annotationId, oldText, newText)) {
        QMessageBox::warning(this, "Edit added text", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    refreshAfterPageEdit({ page }, page);
}

void MainWindow::deleteTextAnnotation(int page, int annotationId, const QString &text)
{
    if (!m_editor)
        return;

    if (!m_editor->deleteTextAnnotation(page, annotationId, text)) {
        QMessageBox::warning(this, "Delete added text", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return;
    }
    if (refreshAfterPageEdit({ page }, page))
        statusBar()->showMessage("Deleted added text.", 3000);
}

void MainWindow::updateTextAnnotationBounds(int page, int annotationId,
                                            const QString &text, const QRectF &bounds)
{
    if (!m_editor)
        return;

    if (!m_editor->setTextAnnotationBounds(page, annotationId, text, bounds)) {
        QMessageBox::warning(this, "Update text box failed", m_editor->lastError());
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
        "Click on the page where the image goes. Drag to move or resize. Use Crop to crop, "
        "Enter or a click elsewhere places it, Cancel or Esc stops placement.", 12000);
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

bool MainWindow::fillFormField(int page, const QPointF &point, PageView::FormField *result)
{
    if (!m_editor || !result)
        return false;

    PdfEditor::FormField field;
    if (!m_editor->formFieldAt(page, point, &field)) {
        QMessageBox::warning(this, "Fill form failed", m_editor->lastError());
        return true;
    }
    if (field.type == PdfEditor::FormFieldType::None)
        return false;

    result->bounds = field.bounds;
    result->value = field.value;
    result->options = field.options;
    result->optionValues = field.optionValues;
    result->checked = field.checked;
    result->readOnly = field.readOnly;
    result->multiline = field.multiline;
    result->password = field.password;
    result->editableChoice = field.editableChoice;
    result->widgetIndex = field.widgetIndex;
    if (field.readOnly) {
        const QString fieldName = field.label.isEmpty() ? field.name : field.label;
        statusBar()->showMessage(fieldName.isEmpty() ? "This form field is read-only."
                                                      : fieldName + " is read-only.",
                                 4000);
        return true;
    }

    switch (field.type) {
    case PdfEditor::FormFieldType::Text:
        result->type = PageView::FormField::Type::Text;
        return true;
    case PdfEditor::FormFieldType::ComboBox:
        result->type = PageView::FormField::Type::ComboBox;
        return true;
    case PdfEditor::FormFieldType::ListBox:
        result->type = PageView::FormField::Type::ListBox;
        return true;
    case PdfEditor::FormFieldType::CheckBox:
        result->type = PageView::FormField::Type::CheckBox;
        break;
    case PdfEditor::FormFieldType::RadioButton:
        result->type = PageView::FormField::Type::RadioButton;
        break;
    case PdfEditor::FormFieldType::None:
        return false;
    }

    if (!m_editor->setFormFieldValue(page, field.widgetIndex, QString())) {
        QMessageBox::warning(this, "Fill form failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return true;
    }
    refreshAfterEdit(page);
    return true;
}

bool MainWindow::commitFormField(int page, int widgetIndex, const QString &value)
{
    if (!m_editor)
        return false;
    if (!m_editor->setFormFieldValue(page, widgetIndex, value)) {
        QMessageBox::warning(this, "Fill form failed", m_editor->lastError());
        reopenEditor(m_history.current());
        updateActions();
        return false;
    }
    return refreshAfterEdit(page);
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

    m_view->clearFormFieldOverlays();
    if (!reopenEditor(path)) {
        QMessageBox::warning(this, "Editing", "Editing is no longer available for this file.");
    } else {
        refreshFormFieldOverlays();
    }

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
    return m_history.isModified() || m_view->hasPendingChanges()
           || m_passwordChange != PasswordChange::None;
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
    if (!reopenEditor(working) || !shown)
        return shown;

    refreshFormFieldOverlays();
    return shown;
}

bool MainWindow::refreshFormFieldOverlays()
{
    if (!m_editor || !m_document
        || m_document->status() != QPdfDocument::Status::Ready)
        return false;

    for (int pageIndex = 0; pageIndex < m_document->pageCount(); ++pageIndex) {
        QVector<PdfEditor::FormField> editorFields;
        if (!m_editor->formFieldsOnPage(pageIndex, &editorFields)) {
            QMessageBox::warning(this, "Form fields",
                                 "Could not load form fields for display: "
                                     + m_editor->lastError());
            m_view->clearFormFieldOverlays();
            return false;
        }
        QVector<PageView::FormField> viewFields;
        viewFields.reserve(editorFields.size());
        for (const PdfEditor::FormField &editorField : editorFields) {
            PageView::FormField viewField;
            switch (editorField.type) {
            case PdfEditor::FormFieldType::Text:
                viewField.type = PageView::FormField::Type::Text;
                break;
            case PdfEditor::FormFieldType::CheckBox:
                viewField.type = PageView::FormField::Type::CheckBox;
                break;
            case PdfEditor::FormFieldType::RadioButton:
                viewField.type = PageView::FormField::Type::RadioButton;
                break;
            case PdfEditor::FormFieldType::ComboBox:
                viewField.type = PageView::FormField::Type::ComboBox;
                break;
            case PdfEditor::FormFieldType::ListBox:
                viewField.type = PageView::FormField::Type::ListBox;
                break;
            case PdfEditor::FormFieldType::None:
                continue;
            }
            viewField.bounds = editorField.bounds;
            viewField.value = editorField.value;
            viewField.options = editorField.options;
            viewField.optionValues = editorField.optionValues;
            viewField.checked = editorField.checked;
            viewField.readOnly = editorField.readOnly;
            viewField.multiline = editorField.multiline;
            viewField.password = editorField.password;
            viewField.editableChoice = editorField.editableChoice;
            viewField.widgetIndex = editorField.widgetIndex;
            viewFields.append(viewField);
        }
        m_view->setFormFields(pageIndex, viewFields);
    }
    return true;
}