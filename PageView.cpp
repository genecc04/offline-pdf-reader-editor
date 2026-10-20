#include "PageView.h"

#include <QAbstractItemModel>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QPdfLink>
#include <QPdfSearchModel>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QScrollBar>
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
}

// document and zoom

void PageView::setDocument(QPdfDocument *document)
{
    m_document = document;
    connect(m_document, &QPdfDocument::statusChanged, this, [this]() { onDocumentChanged(); });
    onDocumentChanged();
}

void PageView::onDocumentChanged()
{
    finishEditor(false);

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
    const QImage image = m_document->render(page, size * dpr, options);

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

        for (const Stroke &stroke : std::as_const(m_strokes)) {
            if (stroke.page == page)
                drawStroke(painter, stroke, target);
        }
        if (m_drawing && m_activeStroke.page == page)
            drawStroke(painter, m_activeStroke, target);
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

// draw tool

void PageView::setDrawToolActive(bool active)
{
    if (!active)
        commitStrokes();
    m_drawTool = active;
    m_drawing = false;
    if (active)
        viewport()->setCursor(Qt::CrossCursor);
    else if (!m_textTool)
        viewport()->setCursor(Qt::ArrowCursor);
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

void PageView::commitPendingEdits()
{
    finishEditor(true);
    commitStrokes();
}

// text tool

void PageView::setTextToolActive(bool active)
{
    if (!active)
        finishEditor(true);
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

void PageView::mousePressEvent(QMouseEvent *event)
{
    if (m_editor) {
        finishEditor(true);
        return;
    }
    if (m_textTool && event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragStart = event->position().toPoint();
        m_dragRect = QRect();
        return;
    }
    if (m_drawTool && event->button() == Qt::LeftButton) {
        const QPoint pos = event->position().toPoint();
        const int page = pageAtViewportPoint(pos);
        if (page >= 0) {
            m_activeStroke = Stroke();
            m_activeStroke.page = page;
            m_activeStroke.style = m_drawStyle;
            m_activeStroke.points << pagePointFor(page, pos);
            m_drawing = true;
        }
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drawing) {
        const QPointF point = pagePointFor(m_activeStroke.page, event->position().toPoint());
        if (QLineF(m_activeStroke.points.last(), point).length() * m_scale >= 1.5)
            m_activeStroke.points << point;
        viewport()->update();
        return;
    }
    if (m_dragging) {
        m_dragRect = QRect(m_dragStart, event->position().toPoint()).normalized();
        viewport()->update();
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
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

    QString family = QStringLiteral("Arial");
    if (m_style.pdfFont == QLatin1String("TiRo"))
        family = QStringLiteral("Times New Roman");
    else if (m_style.pdfFont == QLatin1String("Cour"))
        family = QStringLiteral("Courier New");

    m_editor->setStyleSheet(
        QStringLiteral("QPlainTextEdit { color: %1; background: rgba(255,255,255,215);"
                       " border: 2px solid #0066cc; font-family: \"%2\"; font-size: %3px; }")
            .arg(m_style.color.name(), family)
            .arg(qMax(4, int(m_style.sizePoints * m_scale))));
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