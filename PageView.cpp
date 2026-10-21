#include "PageView.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QPdfLink>
#include <QPdfSearchModel>
#include <QPdfSelection>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QScrollBar>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>

#include <utility>

namespace {
constexpr int kMargin = 12;
constexpr int kSpacing = 12;
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 8.0;
}

PageView::PageView(QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_cache(256 * 1024)
{
    setFocusPolicy(Qt::StrongFocus);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    horizontalScrollBar()->setSingleStep(30);
    verticalScrollBar()->setSingleStep(40);
    viewport()->setMouseTracking(true);

    m_autoScroll = new QTimer(this);
    m_autoScroll->setInterval(30);
    connect(m_autoScroll, &QTimer::timeout, this, [this]() {
        if (!m_selecting) {
            m_autoScroll->stop();
            return;
        }
        int dy = 0;
        if (m_lastMouse.y() < 0)
            dy = m_lastMouse.y() / 2 - 2;
        else if (m_lastMouse.y() > viewport()->height())
            dy = (m_lastMouse.y() - viewport()->height()) / 2 + 2;
        if (dy != 0) {
            verticalScrollBar()->setValue(verticalScrollBar()->value() + qBound(-40, dy, 40));
            extendSelection(m_lastMouse);
        }
    });
}

// document and zoom

void PageView::setDocument(QPdfDocument *document)
{
    m_document = document;
    connect(m_document, &QPdfDocument::statusChanged, this, [this]() { onDocumentChanged(); });
    onDocumentChanged();
}

void PageView::setPageRenderDocument(QPdfDocument *document,
                                     const QList<int> &changedPages)
{
    m_pageRenderDocument = document;
    if (changedPages.isEmpty()) {
        m_cache.clear();
        viewport()->update();
        return;
    }

    const QList<quint64> keys = m_cache.keys();
    for (quint64 key : keys) {
        if (changedPages.contains(int(key >> 40)))
            m_cache.remove(key);
    }

    const QPoint offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
    for (int page : changedPages) {
        if (page >= 0 && page < m_pageRects.size())
            viewport()->update(m_pageRects.at(page).translated(-offset));
    }
}

void PageView::onDocumentChanged()
{
    finishEditor(false);
    finishImage(false);

    m_selecting = false;
    m_selection.clear();
    m_selStartPage = -1;
    m_selEndPage = -1;
    m_erasing = false;
    m_erasePage = -1;
    m_erasePoints.clear();

    const bool hadStrokes = !m_strokes.isEmpty();
    m_strokes.clear();
    m_drawing = false;
    m_activeStroke = Stroke();
    if (hadStrokes)
        emit pendingStrokesChanged();

    m_cache.clear();
    m_pageSizes.clear();

    if (m_document && m_document->status() == QPdfDocument::Status::Ready) {
        const int count = m_document->pageCount();
        m_pageSizes.reserve(count);
        for (int i = 0; i < count; ++i)
            m_pageSizes.append(m_document->pagePointSize(i));
    }
    recalculateLayout(false);
}

void PageView::setZoomMode(ZoomMode mode)
{
    m_zoomMode = mode;
    recalculateLayout(true);
}

void PageView::setZoomFactor(double factor)
{
    m_customZoom = qBound(kMinZoom, factor, kMaxZoom);
    m_zoomMode = ZoomMode::Custom;
    recalculateLayout(true);
}

double PageView::zoomFactor() const
{
    return m_scale / (logicalDpiX() / 72.0);
}

void PageView::recalculateLayout(bool keepPosition)
{
    int anchorPage = -1;
    double anchorFraction = 0.0;
    if (keepPosition && !m_pageRects.isEmpty()) {
        anchorPage = qBound(0, m_currentPage, int(m_pageRects.size()) - 1);
        const QRect &r = m_pageRects.at(anchorPage);
        anchorFraction = double(verticalScrollBar()->value() - r.top()) / qMax(1, r.height());
    }

    m_pageRects.clear();
    m_documentSize = QSize();

    if (!m_pageSizes.isEmpty()) {
        double maxWidth = 1.0;
        double maxHeight = 1.0;
        for (const QSizeF &size : std::as_const(m_pageSizes)) {
            maxWidth = qMax(maxWidth, size.width());
            maxHeight = qMax(maxHeight, size.height());
        }

        const int viewWidth = viewport()->width();
        const int viewHeight = viewport()->height();
        switch (m_zoomMode) {
        case ZoomMode::Custom:
            m_scale = m_customZoom * logicalDpiX() / 72.0;
            break;
        case ZoomMode::FitToWidth:
            m_scale = (viewWidth - 2.0 * kMargin) / maxWidth;
            break;
        case ZoomMode::FitInView:
            m_scale = qMin((viewWidth - 2.0 * kMargin) / maxWidth,
                           (viewHeight - 2.0 * kMargin) / maxHeight);
            break;
        }
        m_scale = qMax(m_scale, 0.01);

        QVector<QSize> pixelSizes;
        pixelSizes.reserve(m_pageSizes.size());
        int widest = 0;
        for (const QSizeF &size : std::as_const(m_pageSizes)) {
            const QSize px(qMax(1, qRound(size.width() * m_scale)),
                           qMax(1, qRound(size.height() * m_scale)));
            pixelSizes.append(px);
            widest = qMax(widest, px.width());
        }

        const int contentWidth = qMax(widest + 2 * kMargin, viewWidth);
        int y = kMargin;
        for (const QSize &px : std::as_const(pixelSizes)) {
            m_pageRects.append(QRect((contentWidth - px.width()) / 2, y,
                                     px.width(), px.height()));
            y += px.height() + kSpacing;
        }
        m_documentSize = QSize(contentWidth, y - kSpacing + kMargin);
    }

    horizontalScrollBar()->setRange(0, qMax(0, m_documentSize.width() - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    verticalScrollBar()->setRange(0, qMax(0, m_documentSize.height() - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());

    if (anchorPage >= 0 && anchorPage < m_pageRects.size()) {
        const QRect &r = m_pageRects.at(anchorPage);
        verticalScrollBar()->setValue(r.top() + int(anchorFraction * r.height()));
    } else if (!keepPosition) {
        verticalScrollBar()->setValue(0);
        horizontalScrollBar()->setValue(0);
    }

    applyEditorStyle();
    positionEditor();
    viewport()->update();
    updateCurrentPage();
}

// navigation

void PageView::updateCurrentPage()
{
    if (m_pageRects.isEmpty())
        return;

    const int middle = verticalScrollBar()->value() + viewport()->height() / 2;
    int page = 0;
    for (int i = 0; i < m_pageRects.size(); ++i) {
        if (m_pageRects.at(i).top() <= middle)
            page = i;
        else
            break;
    }
    if (page != m_currentPage) {
        m_currentPage = page;
        emit currentPageChanged(page);
    }
}

void PageView::jumpToPage(int page, const QPointF &location)
{
    if (page < 0 || page >= m_pageRects.size())
        return;
    const QRect &r = m_pageRects.at(page);

    m_jumping = true;
    if (location.isNull()) {
        verticalScrollBar()->setValue(r.top() - kMargin);
    } else {
        // Bring the spot a third of the way down the window.
        verticalScrollBar()->setValue(
            r.top() + int(location.y() * m_scale) - viewport()->height() / 3);
        horizontalScrollBar()->setValue(
            r.left() + int(location.x() * m_scale) - viewport()->width() / 2);
    }
    m_jumping = false;

    if (page != m_currentPage) {
        m_currentPage = page;
        emit currentPageChanged(page);
    }
    viewport()->update();
}

int PageView::pageAtViewportPoint(const QPoint &point) const
{
    const QPoint doc = point + QPoint(horizontalScrollBar()->value(),
                                      verticalScrollBar()->value());
    for (int i = 0; i < m_pageRects.size(); ++i) {
        if (m_pageRects.at(i).contains(doc))
            return i;
    }
    return -1;
}

int PageView::nearestPage(const QPoint &viewportPos) const
{
    const int y = viewportPos.y() + verticalScrollBar()->value();
    for (int i = 0; i < m_pageRects.size(); ++i) {
        if (y <= m_pageRects.at(i).bottom())
            return i;
    }
    return qMax(0, int(m_pageRects.size()) - 1);
}

QRect PageView::pageRectInViewport(int page) const
{
    return m_pageRects.at(page).translated(-horizontalScrollBar()->value(),
                                           -verticalScrollBar()->value());
}

QPointF PageView::pagePointFor(int page, const QPoint &viewportPos) const
{
    // Screen pixels to points on the page, kept inside the page.
    const QRect r = pageRectInViewport(page);
    const QSizeF size = m_pageSizes.at(page);
    return QPointF(qBound(0.0, (viewportPos.x() - r.x()) / m_scale, size.width()),
                   qBound(0.0, (viewportPos.y() - r.y()) / m_scale, size.height()));
}

// search results

void PageView::setSearchModel(QPdfSearchModel *model)
{
    m_search = model;
    if (!m_search)
        return;
    auto repaint = [this]() { viewport()->update(); };
    connect(m_search, &QAbstractItemModel::modelReset, this, repaint);
    connect(m_search, &QAbstractItemModel::rowsInserted, this, repaint);
}

void PageView::setCurrentSearchResultIndex(int index)
{
    m_searchIndex = index;
    viewport()->update();
}

void PageView::drawSearchResults(QPainter &painter, int page, const QRect &pageRect,
                                 const QPdfLink &current)
{
    if (!m_search)
        return;
    const QList<QPdfLink> results = m_search->resultsOnPage(page);
    for (const QPdfLink &link : results) {
        const bool isCurrent = current.isValid() && current.page() == page
                               && current.location() == link.location();
        const QList<QRectF> rects = link.rectangles();
        for (const QRectF &r : rects) {
            const QRectF px(pageRect.x() + r.x() * m_scale, pageRect.y() + r.y() * m_scale,
                            r.width() * m_scale, r.height() * m_scale);
            painter.fillRect(px, isCurrent ? QColor(255, 140, 0, 110) : QColor(255, 235, 0, 100));
            if (isCurrent) {
                painter.setPen(QPen(QColor(255, 100, 0), 2));
                painter.drawRect(px);
            }
        }
    }
}

// painting

QPixmap PageView::pageImage(int page, const QSize &size, qreal dpr)
{
    const quint64 key = (quint64(page) << 40) | (quint64(size.width()) << 20)
                        | quint64(size.height());
    if (QPixmap *cached = m_cache.object(key))
        return *cached;

    QPdfDocumentRenderOptions options;
    options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
    QPdfDocument *source = m_pageRenderDocument
                                   && m_pageRenderDocument->status() == QPdfDocument::Status::Ready
                               ? m_pageRenderDocument
                               : m_document;
    const QImage image = source->render(page, size * dpr, options);

    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(dpr);
    m_cache.insert(key, new QPixmap(pixmap), pixmap.width() * pixmap.height() * 4 / 1024);
    return pixmap;
}

void PageView::drawStroke(QPainter &painter, const Stroke &stroke, const QRect &pageRect)
{
    if (stroke.points.isEmpty())
        return;

    QPolygonF polygon;
    for (const QPointF &p : stroke.points)
        polygon << QPointF(pageRect.x() + p.x() * m_scale, pageRect.y() + p.y() * m_scale);

    QColor color = stroke.style.color;
    color.setAlphaF(stroke.style.opacity);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, qMax(1.0, stroke.style.widthPoints * m_scale),
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (polygon.size() == 1)
        painter.drawPoint(polygon.first());
    else
        painter.drawPolyline(polygon);
    painter.restore();
}

void PageView::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(), QColor(70, 70, 70));
    if (m_pageRects.isEmpty() || !m_document)
        return;

    const QPoint offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
    const QRect visible(offset, viewport()->size());
    const qreal dpr = devicePixelRatioF();

    QPdfLink current;
    if (m_search && m_searchIndex >= 0
        && m_searchIndex < m_search->rowCount(QModelIndex())) {
        current = m_search->resultAtIndex(m_searchIndex);
    }

    for (int page = 0; page < m_pageRects.size(); ++page) {
        const QRect &r = m_pageRects.at(page);
        if (r.top() > visible.bottom())
            break;
        if (!r.intersects(visible))
            continue;

        const QRect target = r.translated(-offset);
        painter.fillRect(target, Qt::white);
        painter.drawPixmap(target.topLeft(), pageImage(page, target.size(), dpr));
        drawSearchResults(painter, page, target, current);

        painter.save();
        painter.setClipRect(target, Qt::IntersectClip);

        for (const SelectionPart &part : std::as_const(m_selection)) {
            if (part.page != page)
                continue;
            painter.save();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 120, 215, 90));
            for (const QPolygonF &polygon : part.bounds) {
                QPolygonF mapped;
                for (const QPointF &p : polygon)
                    mapped << QPointF(target.x() + p.x() * m_scale,
                                      target.y() + p.y() * m_scale);
                painter.drawPolygon(mapped);
            }
            painter.restore();
        }

        for (const Stroke &stroke : std::as_const(m_strokes)) {
            if (stroke.page == page)
                drawStroke(painter, stroke, target);
        }
        if (m_drawing && m_activeStroke.page == page)
            drawStroke(painter, m_activeStroke, target);

        // An image being placed.
        if (m_imageActive && m_imagePage == page)
            drawImagePlacement(painter, target);

        if (m_drawTool && m_eraserActive && m_erasePage == page) {
            const QPointF center(m_eraserCursorPos);
            const qreal radius = qMax(4.0, m_drawStyle.widthPoints * m_scale / 2.0);
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(40, 40, 40, 190), 1));
            painter.setBrush(QColor(255, 255, 255, 100));
            painter.drawEllipse(center, radius, radius);
            painter.restore();
        }

        painter.restore();
    }

    if (m_dragging && !m_dragRect.isNull()) {
        painter.setPen(QPen(QColor(0, 102, 204), 2, Qt::DashLine));
        painter.setBrush(QColor(0, 102, 204, 40));
        painter.drawRect(m_dragRect);
    }
}

void PageView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    recalculateLayout(true);
}

void PageView::scrollContentsBy(int dx, int dy)
{
    QAbstractScrollArea::scrollContentsBy(dx, dy);
    positionEditor();
    if (!m_jumping)
        updateCurrentPage();
}

void PageView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        setZoomFactor(zoomFactor() * (event->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1));
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    if (m_imageAwaiting || m_imageActive) {
        if (event->key() == Qt::Key_Escape) {
            finishImage(false);
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            finishImage(true);
            return;
        }
    }

    if (event->matches(QKeySequence::Copy) && !m_selection.isEmpty()) {
        copySelection();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAllText();
        return;
    }
    if (event->key() == Qt::Key_Escape && !m_selection.isEmpty()) {
        clearSelection();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PageView::commitPendingEdits()
{
    finishEditor(true);
    commitStrokes();
    finishImage(true);
}

// text selection

void PageView::updateSelection()
{
    m_selection.clear();
    if (!m_document || m_selStartPage < 0 || m_selEndPage < 0)
        return;

    int firstPage = m_selStartPage;
    int lastPage = m_selEndPage;
    QPointF a = m_selStartPoint;
    QPointF b = m_selEndPoint;
    const bool sameLine = qAbs(b.y() - a.y()) < 3.0;
    if (lastPage < firstPage
        || (lastPage == firstPage && (b.y() < a.y() || (sameLine && b.x() < a.x())))) {
        std::swap(firstPage, lastPage);
        std::swap(a, b);
    }

    for (int page = firstPage; page <= lastPage; ++page) {
        const QSizeF size = m_pageSizes.at(page);
        const QPointF start = (page == firstPage) ? a : QPointF(0, 0);
        const QPointF end = (page == lastPage) ? b : QPointF(size.width(), size.height());

        const QPdfSelection selection = m_document->getSelection(page, start, end);
        if (selection.isValid() && !selection.text().isEmpty()) {
            SelectionPart part;
            part.page = page;
            part.bounds = selection.bounds();
            part.text = selection.text();
            m_selection.append(part);
        }
    }
}

void PageView::extendSelection(const QPoint &viewportPos)
{
    if (!m_selMoved) {
        if ((viewportPos - m_selPressPos).manhattanLength() < QApplication::startDragDistance())
            return;
        m_selMoved = true;
    }

    int page = pageAtViewportPoint(viewportPos);
    if (page < 0)
        page = nearestPage(viewportPos);
    m_selEndPage = page;
    m_selEndPoint = pagePointFor(page, viewportPos);

    updateSelection();
    viewport()->update();
}

void PageView::clearSelection()
{
    m_selStartPage = -1;
    m_selEndPage = -1;
    if (m_selection.isEmpty())
        return;
    m_selection.clear();
    viewport()->update();
}

void PageView::selectAllText()
{
    if (m_pageSizes.isEmpty())
        return;
    const int last = int(m_pageSizes.size()) - 1;
    m_selStartPage = 0;
    m_selStartPoint = QPointF(0, 0);
    m_selEndPage = last;
    m_selEndPoint = QPointF(m_pageSizes.at(last).width(), m_pageSizes.at(last).height());

    updateSelection();
    viewport()->update();
    if (!m_selection.isEmpty())
        emit textSelected();
}

void PageView::copySelection()
{
    if (m_selection.isEmpty())
        return;

    QStringList parts;
    for (const SelectionPart &part : std::as_const(m_selection))
        parts << part.text;

    QString text = parts.join(QLatin1Char('\n'));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QGuiApplication::clipboard()->setText(text);
}

void PageView::contextMenuEvent(QContextMenuEvent *event)
{
    if (m_textTool || m_drawTool || m_editor || m_imageAwaiting || m_imageActive)
        return;

    QMenu menu(this);
    QAction *copy = menu.addAction("Copy");
    copy->setShortcut(QKeySequence::Copy);
    copy->setEnabled(!m_selection.isEmpty());
    QAction *all = menu.addAction("Select all");
    all->setShortcut(QKeySequence::SelectAll);

    const QAction *chosen = menu.exec(event->globalPos());
    if (chosen == copy)
        copySelection();
    else if (chosen == all)
        selectAllText();
}

// draw tool

void PageView::setDrawToolActive(bool active)
{
    if (!active)
        commitStrokes();
    else
        clearSelection();
    m_drawTool = active;
    m_drawing = false;
    if (active)
        viewport()->setCursor(Qt::CrossCursor);
    else if (!m_textTool)
        viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();
}

void PageView::setEraserActive(bool active)
{
    m_eraserActive = active;
    m_erasing = false;
    m_erasePoints.clear();
    m_erasePage = -1;
    viewport()->setCursor(m_drawTool ? Qt::CrossCursor : Qt::ArrowCursor);
    viewport()->update();
}

void PageView::setDrawStyle(const DrawStyle &style)
{
    m_drawStyle = style;
}

bool PageView::undoLastStroke()
{
    if (m_strokes.isEmpty())
        return false;
    m_strokes.removeLast();
    viewport()->update();
    emit pendingStrokesChanged();
    return true;
}

void PageView::commitStrokes()
{
    if (m_strokes.isEmpty())
        return;
    const QVector<Stroke> strokes = m_strokes;
    m_strokes.clear();
    viewport()->update();
    emit pendingStrokesChanged();
    emit strokesPlaced(strokes);
}

// image placement

void PageView::beginImagePlacement(const QImage &image)
{
    finishImage(false);
    if (image.isNull())
        return;
    clearSelection();
    m_pendingImage = image;
    m_imageAwaiting = true;
    viewport()->setCursor(Qt::CrossCursor);
    setFocus();
}

void PageView::finishImage(bool commit)
{
    if (!m_imageAwaiting && !m_imageActive)
        return;

    const bool wasActive = m_imageActive;
    const QImage image = m_pendingImage;
    const int page = m_imagePage;
    const QRectF box = m_imageRect;

    m_imageAwaiting = false;
    m_imageActive = false;
    m_imageDrag = ImageDrag::None;
    m_pendingImage = QImage();
    viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();

    if (commit && wasActive)
        emit imagePlaced(page, box, image);
}

QRectF PageView::imagePixelRect() const
{
    const QRect page = pageRectInViewport(m_imagePage);
    return QRectF(page.x() + m_imageRect.x() * m_scale, page.y() + m_imageRect.y() * m_scale,
                  m_imageRect.width() * m_scale, m_imageRect.height() * m_scale);
}

int PageView::imageHandleAt(const QPoint &viewportPos) const
{
    if (m_imagePage < 0 || m_imagePage >= m_pageRects.size())
        return -1;
    const QRectF r = imagePixelRect();
    const QPointF corners[4] = { r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() };
    for (int i = 0; i < 4; ++i) {
        if (QLineF(corners[i], QPointF(viewportPos)).length() <= 12.0)
            return i;
    }
    return -1;
}

void PageView::updateImageCursor(const QPoint &viewportPos)
{
    const int handle = imageHandleAt(viewportPos);
    if (handle == 0 || handle == 3)
        viewport()->setCursor(Qt::SizeFDiagCursor);
    else if (handle == 1 || handle == 2)
        viewport()->setCursor(Qt::SizeBDiagCursor);
    else if (imagePixelRect().contains(QPointF(viewportPos)))
        viewport()->setCursor(Qt::SizeAllCursor);
    else
        viewport()->setCursor(Qt::ArrowCursor);
}

void PageView::drawImagePlacement(QPainter &painter, const QRect &pageRect)
{
    const QRectF px(pageRect.x() + m_imageRect.x() * m_scale,
                    pageRect.y() + m_imageRect.y() * m_scale,
                    m_imageRect.width() * m_scale, m_imageRect.height() * m_scale);

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(px, m_pendingImage);

    painter.setPen(QPen(QColor(0, 102, 204), 2, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(px);

    painter.setPen(QPen(QColor(0, 102, 204), 1));
    painter.setBrush(Qt::white);
    const QPointF corners[4] = { px.topLeft(), px.topRight(), px.bottomLeft(), px.bottomRight() };
    for (const QPointF &c : corners)
        painter.drawRect(QRectF(c.x() - 5, c.y() - 5, 10, 10));
    painter.restore();
}

// text tool

void PageView::setTextToolActive(bool active)
{
    if (!active)
        finishEditor(true);
    else
        clearSelection();
    m_textTool = active;
    m_dragging = false;
    m_dragRect = QRect();
    if (active)
        viewport()->setCursor(Qt::IBeamCursor);
    else if (!m_drawTool)
        viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();
}

void PageView::setTextStyle(const TextStyle &style)
{
    m_style = style;
    applyEditorStyle();
}

// mouse

void PageView::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();

    if (m_editor) {
        finishEditor(true);
        return;
    }

    if (m_imageActive) {
        if (event->button() != Qt::LeftButton)
            return;
        const int handle = imageHandleAt(pos);
        if (handle >= 0) {
            m_imageDrag = ImageDrag::Resize;
            m_imageHandle = handle;
            switch (handle) {
            case 0: m_imageAnchor = m_imageRect.bottomRight(); break;
            case 1: m_imageAnchor = m_imageRect.bottomLeft(); break;
            case 2: m_imageAnchor = m_imageRect.topRight(); break;
            default: m_imageAnchor = m_imageRect.topLeft(); break;
            }
            return;
        }
        if (imagePixelRect().contains(QPointF(pos))) {
            m_imageDrag = ImageDrag::Move;
            m_imageGrab = pagePointFor(m_imagePage, pos) - m_imageRect.topLeft();
            return;
        }
        finishImage(true);
        return;
    }

    if (m_imageAwaiting) {
        if (event->button() != Qt::LeftButton)
            return;
        const int page = pageAtViewportPoint(pos);
        if (page < 0)
            return;

        const QSizeF pageSize = m_pageSizes.at(page);
        const double aspect = double(m_pendingImage.width()) / qMax(1, m_pendingImage.height());
        double width = qMax(30.0, qMin(m_pendingImage.width() * 0.75, pageSize.width() * 0.4));
        double height = width / aspect;
        if (height > pageSize.height() * 0.5) {
            height = pageSize.height() * 0.5;
            width = height * aspect;
        }

        const QPointF center = pagePointFor(page, pos);
        QRectF rect(center.x() - width / 2, center.y() - height / 2, width, height);
        rect.moveLeft(qBound(0.0, rect.left(), qMax(0.0, pageSize.width() - width)));
        rect.moveTop(qBound(0.0, rect.top(), qMax(0.0, pageSize.height() - height)));

        m_imagePage = page;
        m_imageRect = rect;
        m_imageAwaiting = false;
        m_imageActive = true;
        viewport()->update();
        return;
    }

    if (m_textTool && event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragStart = pos;
        m_dragRect = QRect();
        return;
    }
    if (m_drawTool && event->button() == Qt::LeftButton) {
        const int page = pageAtViewportPoint(pos);
        if (m_eraserActive) {
            if (page >= 0) {
                m_erasing = true;
                m_erasePage = page;
                m_eraserCursorPos = pos;
                m_erasePoints.clear();
                m_erasePoints << pagePointFor(page, pos);
                viewport()->update();
            }
            return;
        }
        if (page >= 0) {
            m_activeStroke = Stroke();
            m_activeStroke.page = page;
            m_activeStroke.style = m_drawStyle;
            m_activeStroke.points << pagePointFor(page, pos);
            m_drawing = true;
        }
        return;
    }

    if (event->button() == Qt::LeftButton && !m_textTool && !m_drawTool) {
        setFocus();
        const int page = pageAtViewportPoint(pos);
        const bool extending = page >= 0 && (event->modifiers() & Qt::ShiftModifier)
                               && !m_selection.isEmpty() && m_selStartPage >= 0;
        if (!extending)
            clearSelection();
        if (page >= 0) {
            m_selecting = true;
            m_selMoved = extending;
            m_selPressPos = pos;
            m_lastMouse = pos;
            if (!extending) {
                m_selStartPage = page;
                m_selStartPoint = pagePointFor(page, pos);
            }
            m_selEndPage = page;
            m_selEndPoint = m_selStartPoint;
            if (extending) {
                m_selEndPoint = pagePointFor(page, pos);
                updateSelection();
                viewport()->update();
            }
            m_autoScroll->start();
        }
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();

    if (m_imageActive) {
        if (m_imageDrag == ImageDrag::None) {
            updateImageCursor(pos);
            return;
        }

        const QPointF p = pagePointFor(m_imagePage, pos);
        const QSizeF pageSize = m_pageSizes.at(m_imagePage);

        if (m_imageDrag == ImageDrag::Move) {
            QPointF topLeft = p - m_imageGrab;
            topLeft.setX(qBound(0.0, topLeft.x(),
                                qMax(0.0, pageSize.width() - m_imageRect.width())));
            topLeft.setY(qBound(0.0, topLeft.y(),
                                qMax(0.0, pageSize.height() - m_imageRect.height())));
            m_imageRect.moveTopLeft(topLeft);
        } else {
            const double aspect = double(m_pendingImage.width())
                                  / qMax(1, m_pendingImage.height());
            const bool right = (m_imageHandle == 1 || m_imageHandle == 3);
            const bool down = (m_imageHandle == 2 || m_imageHandle == 3);

            double width = right ? p.x() - m_imageAnchor.x() : m_imageAnchor.x() - p.x();
            const double roomX = right ? pageSize.width() - m_imageAnchor.x() : m_imageAnchor.x();
            const double roomY = down ? pageSize.height() - m_imageAnchor.y() : m_imageAnchor.y();
            const double maxWidth = qMin(roomX, roomY * aspect);
            width = qBound(10.0, width, qMax(10.0, maxWidth));
            const double height = width / aspect;

            m_imageRect = QRectF(right ? m_imageAnchor.x() : m_imageAnchor.x() - width,
                                 down ? m_imageAnchor.y() : m_imageAnchor.y() - height,
                                 width, height);
        }
        viewport()->update();
        return;
    }

    if (m_drawTool && m_eraserActive) {
        m_eraserCursorPos = pos;
        if (m_erasing) {
            const QPointF point = pagePointFor(m_erasePage, pos);
            if (QLineF(m_erasePoints.last(), point).length() * m_scale >= 1.5)
                m_erasePoints << point;
        } else {
            m_erasePage = pageAtViewportPoint(pos);
        }
        viewport()->update();
        return;
    }

    if (m_drawing) {
        const QPointF point = pagePointFor(m_activeStroke.page, pos);
        if (QLineF(m_activeStroke.points.last(), point).length() * m_scale >= 1.5)
            m_activeStroke.points << point;
        viewport()->update();
        return;
    }
    if (m_dragging) {
        m_dragRect = QRect(m_dragStart, pos).normalized();
        viewport()->update();
        return;
    }

    if (m_selecting) {
        m_lastMouse = pos;
        extendSelection(pos);
        return;
    }

    if (!m_textTool && !m_drawTool && !m_imageAwaiting && !m_editor)
        viewport()->setCursor(pageAtViewportPoint(pos) >= 0 ? Qt::IBeamCursor : Qt::ArrowCursor);
    QAbstractScrollArea::mouseMoveEvent(event);
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_erasing && event->button() == Qt::LeftButton) {
        const int page = m_erasePage;
        const QVector<QPointF> path = m_erasePoints;
        const double radius = m_drawStyle.widthPoints / 2.0;
        m_erasing = false;
        m_erasePoints.clear();
        viewport()->update();
        if (!path.isEmpty())
            emit eraseRequested(page, path, radius);
        return;
    }

    if (m_selecting && event->button() == Qt::LeftButton) {
        m_selecting = false;
        m_autoScroll->stop();
        if (!m_selection.isEmpty())
            emit textSelected();
        return;
    }

    if (m_imageDrag != ImageDrag::None) {
        m_imageDrag = ImageDrag::None;
        viewport()->update();
        return;
    }

    if (m_drawing && event->button() == Qt::LeftButton) {
        m_drawing = false;
        if (m_activeStroke.points.size() == 1)
            m_activeStroke.points << m_activeStroke.points.first() + QPointF(0.1, 0.0);
        m_strokes.append(m_activeStroke);
        m_activeStroke = Stroke();
        viewport()->update();
        emit pendingStrokesChanged();
        return;
    }

    if (!m_dragging || event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;

    QRect box = QRect(m_dragStart, event->position().toPoint()).normalized();
    m_dragRect = QRect();
    viewport()->update();

    if (box.width() < 12 || box.height() < 12)
        box = QRect(m_dragStart, QSize(int(200 * m_scale), int(60 * m_scale)));

    const int page = pageAtViewportPoint(m_dragStart);
    if (page < 0)
        return;

    const QRect pageRect = pageRectInViewport(page);
    box = box.intersected(pageRect);
    if (box.width() < 12 || box.height() < 12)
        return;

    startEditor(page, QRectF((box.x() - pageRect.x()) / m_scale,
                             (box.y() - pageRect.y()) / m_scale,
                             box.width() / m_scale, box.height() / m_scale));
}

// text editor

void PageView::startEditor(int page, const QRectF &boxPoints)
{
    m_editorPage = page;
    m_editorBox = boxPoints;

    m_editor = new QPlainTextEdit(viewport());
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->installEventFilter(this);
    applyEditorStyle();
    positionEditor();
    m_editor->show();
    m_editor->setFocus();
}

void PageView::applyEditorStyle()
{
    if (!m_editor)
        return;

    const QString background = m_style.color.lightness() > 200
        ? QStringLiteral("rgba(60,60,60,225)")
        : QStringLiteral("rgba(255,255,255,215)");

    m_editor->setStyleSheet(
        QStringLiteral("QPlainTextEdit { color: %1; background: %2; border: 2px solid #0066cc;"
                       " font-family: \"%3\"; font-size: %4px; font-weight: %5; font-style: %6; }")
            .arg(m_style.color.name(), background, m_style.family)
            .arg(qMax(4, int(m_style.sizePoints * m_scale)))
            .arg(m_style.bold ? QStringLiteral("bold") : QStringLiteral("normal"),
                 m_style.italic ? QStringLiteral("italic") : QStringLiteral("normal")));
}

void PageView::positionEditor()
{
    if (!m_editor || m_editorPage < 0 || m_editorPage >= m_pageRects.size())
        return;
    const QRect page = pageRectInViewport(m_editorPage);
    m_editor->setGeometry(page.x() + int(m_editorBox.x() * m_scale),
                          page.y() + int(m_editorBox.y() * m_scale),
                          qMax(40, int(m_editorBox.width() * m_scale)),
                          qMax(24, int(m_editorBox.height() * m_scale)));
}

void PageView::finishEditor(bool commit)
{
    if (!m_editor)
        return;

    QPlainTextEdit *editor = m_editor;
    m_editor = nullptr;

    const QString text = editor->toPlainText().trimmed();
    const int page = m_editorPage;
    const QRectF box = m_editorBox;
    const TextStyle style = m_style;

    editor->hide();
    editor->deleteLater();

    m_textTool = false;
    viewport()->setCursor(Qt::ArrowCursor);
    setFocus();
    emit textToolFinished();

    if (commit && !text.isEmpty())
        emit textPlaced(page, box, text, style);
}

bool PageView::eventFilter(QObject *watched, QEvent *event)
{
    if (m_editor && watched == m_editor) {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                finishEditor(false);
                return true;
            }
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                && (key->modifiers() & Qt::ControlModifier)) {
                finishEditor(true);
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason)
                QTimer::singleShot(0, this, [this]() { finishEditor(true); });
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}