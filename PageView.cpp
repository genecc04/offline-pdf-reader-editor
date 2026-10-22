#include "PageView.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QFont>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
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
#include <QPushButton>
#include <QScrollBar>
#include <QStringList>
#include <QTextCursor>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

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

    m_emptyStateOpenButton = new QPushButton("Open a PDF...", viewport());
    m_emptyStateOpenButton->setObjectName(QStringLiteral("emptyStateOpenButton"));
    m_emptyStateOpenButton->setCursor(Qt::PointingHandCursor);
    m_emptyStateOpenButton->setStyleSheet(
        QStringLiteral("QPushButton {"
                       " color: #344050; background: #e9edf2;"
                       " border: 1px solid #d4dae2; border-radius: 5px;"
                       " padding: 9px 22px; font-weight: 600;"
                       " } QPushButton:hover { background: #e1e6ec; }"
                       " QPushButton:pressed { background: #d8dee6; }"));
    connect(m_emptyStateOpenButton, &QPushButton::clicked,
            this, &PageView::openRequested);

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
    finishFormEditor(false);
    finishImage(false);
    m_resizingTextAnnotation = false;
    m_movingTextAnnotation = false;
    m_textAnnotationResizeCorner = -1;

    clearSelectedTextAnnotation();
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

void PageView::positionEmptyStateButton()
{
    if (!m_emptyStateOpenButton)
        return;
    const QSize buttonSize = m_emptyStateOpenButton->sizeHint();
    m_emptyStateOpenButton->setGeometry(
        (viewport()->width() - buttonSize.width()) / 2,
        viewport()->height() / 2 + 48,
        buttonSize.width(), buttonSize.height());
    const bool loading = m_document
                         && m_document->status() == QPdfDocument::Status::Loading;
    m_emptyStateOpenButton->setVisible(m_pageRects.isEmpty() && !loading);
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
    positionFormEditor();
    positionEmptyStateButton();
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

void PageView::drawFormFieldOverlays(QPainter &painter, int page, const QRect &pageRect)
{
    for (const FormFieldOverlay &overlay : std::as_const(m_formFieldOverlays)) {
        if (overlay.page != page)
            continue;

        const QRectF bounds(
            pageRect.x() + overlay.field.bounds.x() * m_scale,
            pageRect.y() + overlay.field.bounds.y() * m_scale,
            overlay.field.bounds.width() * m_scale,
            overlay.field.bounds.height() * m_scale);
        painter.save();
        painter.setClipRect(pageRect, Qt::IntersectClip);
        painter.fillRect(bounds, Qt::white);
        painter.setPen(QPen(QColor(130, 130, 130), 1));

        QString text = overlay.field.value;
        if (overlay.field.password)
            text = QString(text.size(), QLatin1Char('*'));
        if (overlay.field.type == FormField::Type::ComboBox
            || overlay.field.type == FormField::Type::ListBox) {
            const int valueIndex = overlay.field.optionValues.indexOf(text);
            if (valueIndex >= 0 && valueIndex < overlay.field.options.size())
                text = overlay.field.options.at(valueIndex);
        }

        if (overlay.field.type == FormField::Type::CheckBox) {
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5));
            if (overlay.field.checked) {
                painter.setPen(QPen(Qt::black, qMax(1.0, bounds.width() * 0.09),
                                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawLine(bounds.left() + bounds.width() * 0.22,
                                 bounds.center().y(),
                                 bounds.left() + bounds.width() * 0.43,
                                 bounds.bottom() - bounds.height() * 0.24);
                painter.drawLine(bounds.left() + bounds.width() * 0.43,
                                 bounds.bottom() - bounds.height() * 0.24,
                                 bounds.right() - bounds.width() * 0.16,
                                 bounds.top() + bounds.height() * 0.2);
            }
            painter.restore();
            continue;
        }
        if (overlay.field.type == FormField::Type::RadioButton) {
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(bounds.adjusted(0.5, 0.5, -0.5, -0.5));
            if (overlay.field.checked) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(Qt::black);
                painter.drawEllipse(bounds.adjusted(bounds.width() * 0.27,
                                                    bounds.height() * 0.27,
                                                    -bounds.width() * 0.27,
                                                    -bounds.height() * 0.27));
            }
            painter.restore();
            continue;
        }

        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5));
        const qreal fontSize = qBound(6.0, bounds.height() * 0.58, 18.0);
        QFont font = painter.font();
        font.setPixelSize(qMax(1, qRound(fontSize)));
        painter.setFont(font);
        painter.setPen(Qt::black);
        const qreal arrowWidth = qMin(18.0, qMax(12.0, bounds.height() * 0.9));
        const qreal rightPadding = overlay.field.type == FormField::Type::ComboBox
                                       ? arrowWidth + 3 : 3;
        const QRectF textBounds = bounds.adjusted(3, 1, -rightPadding, -1);
        if (overlay.field.multiline) {
            painter.drawText(textBounds, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
        } else {
            painter.drawText(textBounds, Qt::AlignLeft | Qt::AlignVCenter, text);
        }
        if (overlay.field.type == FormField::Type::ComboBox) {
            const QRectF buttonBounds(bounds.right() - arrowWidth, bounds.top() + 1,
                                      arrowWidth - 1, qMax(0.0, bounds.height() - 2));
            painter.fillRect(buttonBounds, QColor(240, 240, 240));
            painter.setPen(QColor(150, 150, 150));
            painter.drawLine(QPointF(buttonBounds.left(), bounds.top() + 1),
                             QPointF(buttonBounds.left(), bounds.bottom() - 1));

            const QPointF center = buttonBounds.center();
            const qreal halfArrowWidth = qMin(4.0, buttonBounds.width() * 0.28);
            const qreal halfArrowHeight = qMin(2.5, buttonBounds.height() * 0.16);
            QPolygonF arrow;
            arrow << QPointF(center.x() - halfArrowWidth, center.y() - halfArrowHeight)
                  << QPointF(center.x() + halfArrowWidth, center.y() - halfArrowHeight)
                  << QPointF(center.x(), center.y() + halfArrowHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(55, 55, 55));
            painter.drawPolygon(arrow);
        }
        painter.restore();
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
    const bool loading = m_document
                         && m_document->status() == QPdfDocument::Status::Loading;
    if (m_pageRects.isEmpty()) {
        painter.fillRect(event->rect(), QColor(245, 247, 250));
        if (!loading) {
            painter.setPen(QColor(42, 52, 64));
            QFont titleFont = painter.font();
            titleFont.setPixelSize(22);
            titleFont.setWeight(QFont::DemiBold);
            painter.setFont(titleFont);
            const QRect titleRect(0, viewport()->height() / 2 - 66,
                                  viewport()->width(), 34);
            painter.drawText(titleRect, Qt::AlignHCenter | Qt::AlignVCenter,
                             QStringLiteral("No PDF open"));

            QFont hintFont = painter.font();
            hintFont.setPixelSize(14);
            hintFont.setWeight(QFont::Normal);
            painter.setFont(hintFont);
            painter.setPen(QColor(100, 110, 122));
            const QRect hintRect(16, viewport()->height() / 2 - 27,
                                 viewport()->width() - 32, 28);
            painter.drawText(hintRect, Qt::AlignHCenter | Qt::AlignVCenter,
                             QStringLiteral("Drag a PDF here or open one to get started"));
        }
        return;
    }
    painter.fillRect(event->rect(), QColor(70, 70, 70));
    if (!m_document)
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
        drawFormFieldOverlays(painter, page, target);

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

        if (m_selectedTextAnnotationPage == page) {
            const QRectF bounds(
                target.x() + m_selectedTextAnnotationBounds.x() * m_scale,
                target.y() + m_selectedTextAnnotationBounds.y() * m_scale,
                m_selectedTextAnnotationBounds.width() * m_scale,
                m_selectedTextAnnotationBounds.height() * m_scale);
            painter.save();
            painter.setPen(QPen(QColor(0, 102, 204), 2, Qt::DashLine));
            painter.setBrush(QColor(0, 102, 204, 35));
            painter.drawRect(bounds);
            painter.setPen(QPen(QColor(0, 102, 204), 1));
            painter.setBrush(Qt::white);
            const QPointF corners[] = {
                bounds.topLeft(), bounds.topRight(),
                bounds.bottomLeft(), bounds.bottomRight()
            };
            for (const QPointF &corner : corners)
                painter.drawRect(QRectF(corner.x() - 4, corner.y() - 4, 8, 8));
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
    positionFormEditor();
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
    if (event->key() == Qt::Key_Escape && m_selectedTextAnnotationPage >= 0) {
        clearSelectedTextAnnotation();
        return;
    }
    if (event->key() == Qt::Key_Delete && m_selectedTextAnnotationPage >= 0) {
        deleteSelectedTextAnnotation();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PageView::commitPendingEdits()
{
    finishEditor(true);
    finishFormEditor(true);
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

    const int page = pageAtViewportPoint(event->pos());
    int annotationId = 0;
    QRectF annotationBounds;
    QString annotationText;
    TextStyle annotationStyle;
    const bool onAnnotation = page >= 0 && m_textAnnotationHandler
        && m_textAnnotationHandler(page, pagePointFor(page, event->pos()),
                                   &annotationId, &annotationBounds, &annotationText,
                                   &annotationStyle);
    if (onAnnotation) {
        selectTextAnnotation(page, annotationId, annotationBounds,
                             annotationText, annotationStyle);
    } else {
        clearSelectedTextAnnotation();
    }

    QMenu menu(this);
    QAction *editAnnotation = nullptr;
    QAction *deleteAnnotation = nullptr;
    if (m_selectedTextAnnotationPage >= 0) {
        editAnnotation = menu.addAction("Edit added text...");
        deleteAnnotation = menu.addAction("Delete added text");
        menu.addSeparator();
    }
    QAction *copy = menu.addAction("Copy");
    copy->setShortcut(QKeySequence::Copy);
    copy->setEnabled(!m_selection.isEmpty());
    QAction *all = menu.addAction("Select all");
    all->setShortcut(QKeySequence::SelectAll);

    const QAction *chosen = menu.exec(event->globalPos());
    if (chosen == editAnnotation) {
        emit textAnnotationEditRequested(m_selectedTextAnnotationPage,
                                         m_selectedTextAnnotationId,
                                         m_selectedTextAnnotationContents);
    } else if (chosen == deleteAnnotation) {
        deleteSelectedTextAnnotation();
    } else if (chosen == copy) {
        copySelection();
    } else if (chosen == all) {
        selectAllText();
    }
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
    emit pendingChangesChanged();
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
    emit pendingChangesChanged();
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
    emit imagePlacementStateChanged(true, false);
}

void PageView::setImageCropMode(bool active)
{
    if (!m_imageActive || m_imageCropping == active)
        return;

    m_imageCropping = active;
    m_imageCropDragging = false;
    m_imageCropRect = active ? m_imageRect : QRectF();
    viewport()->setCursor(active ? Qt::CrossCursor : Qt::SizeAllCursor);
    emit imageCropModeChanged(active);
    viewport()->update();
}

void PageView::cancelImagePlacement()
{
    finishImage(false);
}

void PageView::finishImage(bool commit)
{
    if (!m_imageAwaiting && !m_imageActive)
        return;

    const bool wasActive = m_imageActive;
    const bool wasCropping = m_imageCropping;
    const QImage image = m_pendingImage;
    const int page = m_imagePage;
    const QRectF box = m_imageRect;

    m_imageAwaiting = false;
    m_imageActive = false;
    m_imageCropping = false;
    m_imageCropDragging = false;
    m_imageCropRect = QRectF();
    m_imageDrag = ImageDrag::None;
    m_pendingImage = QImage();
    viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();

    if (commit && wasActive)
        emit imagePlaced(page, box, image);
    emit imagePlacementStateChanged(false, false);
    if (wasCropping)
        emit imageCropModeChanged(false);
    emit pendingChangesChanged();
}

QRectF PageView::imagePixelRect() const
{
    const QRect page = pageRectInViewport(m_imagePage);
    return QRectF(page.x() + m_imageRect.x() * m_scale, page.y() + m_imageRect.y() * m_scale,
                  m_imageRect.width() * m_scale, m_imageRect.height() * m_scale);
}

void PageView::updateImageCropRect(const QPoint &viewportPos)
{
    const QRect page = pageRectInViewport(m_imagePage);
    const QPointF imagePoint((viewportPos.x() - page.x()) / m_scale,
                             (viewportPos.y() - page.y()) / m_scale);
    const QPointF end(qBound(m_imageRect.left(), imagePoint.x(), m_imageRect.right()),
                      qBound(m_imageRect.top(), imagePoint.y(), m_imageRect.bottom()));
    m_imageCropRect = QRectF(m_imageCropAnchor, end).normalized().intersected(m_imageRect);
    viewport()->update();
}

QRectF PageView::imageCropPixelRect() const
{
    const QRect page = pageRectInViewport(m_imagePage);
    const QRectF crop = m_imageCropRect.isEmpty() ? m_imageRect : m_imageCropRect;
    return QRectF(page.x() + crop.x() * m_scale, page.y() + crop.y() * m_scale,
                  crop.width() * m_scale, crop.height() * m_scale);
}

int PageView::imageCropHandleAt(const QPoint &viewportPos) const
{
    const QRectF rect = imageCropPixelRect();
    const QPointF corners[] = {
        rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()
    };
    for (int i = 0; i < 4; ++i) {
        if (QLineF(corners[i], QPointF(viewportPos)).length() <= 12.0)
            return i;
    }
    return -1;
}

void PageView::applyImageCrop()
{
    if (m_imageCropRect.width() < 3.0 || m_imageCropRect.height() < 3.0) {
        m_imageCropRect = QRectF();
        viewport()->update();
        return;
    }

    const QRectF selected = m_imageCropRect.intersected(m_imageRect);
    if (selected.width() < 3.0 || selected.height() < 3.0) {
        m_imageCropRect = m_imageRect;
        viewport()->update();
        return;
    }
    const double left = (selected.left() - m_imageRect.left()) / m_imageRect.width();
    const double top = (selected.top() - m_imageRect.top()) / m_imageRect.height();
    const double right = (selected.right() - m_imageRect.left()) / m_imageRect.width();
    const double bottom = (selected.bottom() - m_imageRect.top()) / m_imageRect.height();

    const int sourceWidth = m_pendingImage.width();
    const int sourceHeight = m_pendingImage.height();
    const int x = qBound(0, qFloor(left * sourceWidth), sourceWidth - 1);
    const int y = qBound(0, qFloor(top * sourceHeight), sourceHeight - 1);
    const int cropRight = qBound(x + 1, qCeil(right * sourceWidth), sourceWidth);
    const int cropBottom = qBound(y + 1, qCeil(bottom * sourceHeight), sourceHeight);
    const QRect sourceCrop(x, y, cropRight - x, cropBottom - y);
    const QRectF oldPageRect = m_imageRect;
    const QImage cropped = m_pendingImage.copy(sourceCrop);
    if (cropped.isNull())
        return;

    m_imageRect = QRectF(oldPageRect.left() + left * oldPageRect.width(),
                         oldPageRect.top() + top * oldPageRect.height(),
                         (right - left) * oldPageRect.width(),
                         (bottom - top) * oldPageRect.height());
    m_pendingImage = cropped;
    m_imageCropRect = m_imageRect;
    viewport()->setCursor(Qt::CrossCursor);
    emit pendingChangesChanged();
    viewport()->update();
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
    const int handle = m_imageCropping
                           ? imageCropHandleAt(viewportPos) : imageHandleAt(viewportPos);
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

    if (m_imageCropping) {
        const QRectF cropRect = imageCropPixelRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 125));
        painter.drawRect(QRectF(px.left(), px.top(), px.width(), cropRect.top() - px.top()));
        painter.drawRect(QRectF(px.left(), cropRect.bottom(),
                                px.width(), px.bottom() - cropRect.bottom()));
        painter.drawRect(QRectF(px.left(), cropRect.top(),
                                cropRect.left() - px.left(), cropRect.height()));
        painter.drawRect(QRectF(cropRect.right(), cropRect.top(),
                                px.right() - cropRect.right(), cropRect.height()));
        painter.setPen(QPen(Qt::white, 2, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(cropRect);
        painter.setPen(QPen(QColor(0, 102, 204), 1));
        painter.setBrush(Qt::white);
        const QPointF cropCorners[] = {
            cropRect.topLeft(), cropRect.topRight(),
            cropRect.bottomLeft(), cropRect.bottomRight()
        };
        for (const QPointF &corner : cropCorners)
            painter.drawRect(QRectF(corner.x() - 5, corner.y() - 5, 10, 10));
    }

    if (m_imageCropping) {
        painter.restore();
        return;
    }

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
    if (m_editor && !m_editingTextAnnotation) {
        m_editorStyle = style;
        applyEditorStyle();
    }
}

void PageView::setFormFieldHandler(
    const std::function<bool(int, const QPointF &, FormField *)> &handler)
{
    m_formFieldHandler = handler;
}

void PageView::setFormFieldValueHandler(
    const std::function<bool(int, int, const QString &)> &handler)
{
    m_formFieldValueHandler = handler;
}

void PageView::setTextAnnotationHandler(
    const std::function<bool(int, const QPointF &, int *, QRectF *, QString *,
                             TextStyle *)> &handler)
{
    m_textAnnotationHandler = handler;
}

void PageView::editTextAnnotationInline(int page, int annotationId, const QString &text)
{
    if (m_selectedTextAnnotationPage != page
        || m_selectedTextAnnotationId != annotationId)
        return;

    startAnnotationEditor(page, annotationId, m_selectedTextAnnotationBounds,
                          text, m_selectedTextAnnotationStyle);
}

void PageView::deleteSelectedTextAnnotation()
{
    if (m_selectedTextAnnotationPage < 0)
        return;

    if (m_editor && m_editingTextAnnotation)
        finishEditor(false);

    const int page = m_selectedTextAnnotationPage;
    const int annotationId = m_selectedTextAnnotationId;
    const QString text = m_selectedTextAnnotationContents;
    clearSelectedTextAnnotation();
    emit textAnnotationDeleteRequested(page, annotationId, text);
}

void PageView::selectTextAnnotation(int page, int annotationId, const QRectF &bounds,
                                    const QString &text, const TextStyle &style)
{
    const bool wasSelected = m_selectedTextAnnotationPage >= 0;
    m_selectedTextAnnotationPage = page;
    m_selectedTextAnnotationId = annotationId;
    m_selectedTextAnnotationBounds = bounds;
    m_selectedTextAnnotationContents = text;
    m_selectedTextAnnotationStyle = style;
    if (!wasSelected)
        emit textAnnotationSelectionChanged(true);
    viewport()->update();
}

void PageView::clearSelectedTextAnnotation()
{
    if (m_selectedTextAnnotationPage < 0)
        return;
    m_selectedTextAnnotationPage = -1;
    m_selectedTextAnnotationId = 0;
    m_selectedTextAnnotationBounds = QRectF();
    m_selectedTextAnnotationContents.clear();
    m_selectedTextAnnotationStyle = TextStyle();
    emit textAnnotationSelectionChanged(false);
    viewport()->update();
}

QRectF PageView::selectedTextAnnotationViewportBounds() const
{
    if (m_selectedTextAnnotationPage < 0
        || m_selectedTextAnnotationPage >= m_pageRects.size())
        return {};
    const QRect page = pageRectInViewport(m_selectedTextAnnotationPage);
    return QRectF(page.x() + m_selectedTextAnnotationBounds.x() * m_scale,
                  page.y() + m_selectedTextAnnotationBounds.y() * m_scale,
                  m_selectedTextAnnotationBounds.width() * m_scale,
                  m_selectedTextAnnotationBounds.height() * m_scale);
}

int PageView::textAnnotationResizeHandleAt(const QPoint &pos) const
{
    const QRectF bounds = selectedTextAnnotationViewportBounds();
    if (bounds.isEmpty())
        return -1;
    const QPointF corners[] = {
        bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()
    };
    for (int i = 0; i < 4; ++i) {
        if (QRectF(corners[i] - QPointF(7, 7), QSizeF(14, 14)).contains(pos))
            return i;
    }
    return -1;
}

void PageView::setFormFields(int page, const QVector<FormField> &fields)
{
    for (int i = m_formFieldOverlays.size() - 1; i >= 0; --i) {
        if (m_formFieldOverlays.at(i).page == page)
            m_formFieldOverlays.removeAt(i);
    }
    for (const FormField &field : fields) {
        FormFieldOverlay overlay;
        overlay.page = page;
        overlay.field = field;
        switch (field.type) {
        case FormField::Type::Text:
        case FormField::Type::CheckBox:
        case FormField::Type::RadioButton:
        case FormField::Type::ComboBox:
        case FormField::Type::ListBox:
            m_formFieldOverlays.append(overlay);
            break;
        case FormField::Type::None:
            break;
        }
    }
    viewport()->update();
}

void PageView::clearFormFieldOverlays()
{
    if (m_formFieldOverlays.isEmpty())
        return;
    m_formFieldOverlays.clear();
    viewport()->update();
}

bool PageView::hasPendingChanges() const
{
    if (!m_strokes.isEmpty() || m_drawing || m_erasing || m_imageActive)
        return true;
    if (m_editor) {
        const QString text = m_editor->toPlainText();
        if (m_editingTextAnnotation)
            return text != m_editingTextAnnotationOriginal;
        return !text.trimmed().isEmpty();
    }
    return m_formEditor && formEditorValue() != m_formEditorInitialValue;
}

// mouse

void PageView::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();

    if (m_formEditor)
        finishFormEditor(true);

    if (m_editor) {
        finishEditor(true);
        return;
    }

    if (m_imageActive) {
        if (event->button() != Qt::LeftButton)
            return;
        if (m_imageCropping) {
            const int handle = imageCropHandleAt(pos);
            if (handle >= 0) {
                m_imageCropDragging = true;
                switch (handle) {
                case 0: m_imageCropAnchor = m_imageCropRect.bottomRight(); break;
                case 1: m_imageCropAnchor = m_imageCropRect.bottomLeft(); break;
                case 2: m_imageCropAnchor = m_imageCropRect.topRight(); break;
                default: m_imageCropAnchor = m_imageCropRect.topLeft(); break;
                }
            } else {
                m_imageDrag = ImageDrag::Move;
                m_imageGrab = pagePointFor(m_imagePage, pos) - m_imageRect.topLeft();
            }
            return;
        }
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
        emit imagePlacementStateChanged(true, true);
        emit pendingChangesChanged();
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
            emit pendingChangesChanged();
        }
        return;
    }

    if (event->button() == Qt::LeftButton && !m_textTool && !m_drawTool) {
        setFocus();
        if (m_selectedTextAnnotationPage >= 0) {
            const int resizeCorner = textAnnotationResizeHandleAt(pos);
            if (resizeCorner >= 0) {
                m_resizingTextAnnotation = true;
                m_textAnnotationResizeCorner = resizeCorner;
                m_textAnnotationResizeOriginalBounds = m_selectedTextAnnotationBounds;
                return;
            }
            if (selectedTextAnnotationViewportBounds().contains(QPointF(pos))) {
                m_movingTextAnnotation = true;
                m_textAnnotationMoveStart = pagePointFor(
                    m_selectedTextAnnotationPage, pos);
                m_textAnnotationResizeOriginalBounds = m_selectedTextAnnotationBounds;
                return;
            }
        }
        const int page = pageAtViewportPoint(pos);
        int annotationId = 0;
        QRectF annotationBounds;
        QString annotationText;
        TextStyle annotationStyle;
        if (page >= 0 && m_textAnnotationHandler
            && m_textAnnotationHandler(page, pagePointFor(page, pos),
                                       &annotationId, &annotationBounds, &annotationText,
                                       &annotationStyle)) {
            selectTextAnnotation(page, annotationId, annotationBounds,
                                 annotationText, annotationStyle);
            clearSelection();
            return;
        }
        clearSelectedTextAnnotation();
        if (page >= 0 && m_formFieldHandler) {
            FormField field;
            if (m_formFieldHandler(page, pagePointFor(page, pos), &field)) {
                clearSelection();
                if (!field.readOnly
                    && (field.type == FormField::Type::Text
                        || field.type == FormField::Type::ComboBox
                        || field.type == FormField::Type::ListBox))
                    startFormEditor(page, field);
                return;
            }
        }
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

void PageView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_textTool && !m_drawTool && event->button() == Qt::LeftButton) {
        const QPoint pos = event->position().toPoint();
        const int page = pageAtViewportPoint(pos);
        int annotationId = 0;
        QRectF bounds;
        QString text;
        TextStyle style;
        if (page >= 0 && m_textAnnotationHandler
            && m_textAnnotationHandler(page, pagePointFor(page, pos),
                                       &annotationId, &bounds, &text, &style)) {
            selectTextAnnotation(page, annotationId, bounds, text, style);
            emit textAnnotationEditRequested(page, annotationId, text);
            return;
        }
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();

    if (m_resizingTextAnnotation) {
        const QPointF point = pagePointFor(m_selectedTextAnnotationPage, pos);
        const QSizeF pageSize = m_pageSizes.at(m_selectedTextAnnotationPage);
        const qreal x = qBound(0.0, point.x(), pageSize.width());
        const qreal y = qBound(0.0, point.y(), pageSize.height());
        QRectF bounds = m_textAnnotationResizeOriginalBounds;
        constexpr qreal minWidth = 20.0;
        constexpr qreal minHeight = 14.0;
        switch (m_textAnnotationResizeCorner) {
        case 0:
            bounds.setLeft(qMin(x, bounds.right() - minWidth));
            bounds.setTop(qMin(y, bounds.bottom() - minHeight));
            break;
        case 1:
            bounds.setRight(qMax(x, bounds.left() + minWidth));
            bounds.setTop(qMin(y, bounds.bottom() - minHeight));
            break;
        case 2:
            bounds.setLeft(qMin(x, bounds.right() - minWidth));
            bounds.setBottom(qMax(y, bounds.top() + minHeight));
            break;
        case 3:
            bounds.setRight(qMax(x, bounds.left() + minWidth));
            bounds.setBottom(qMax(y, bounds.top() + minHeight));
            break;
        default:
            break;
        }
        m_selectedTextAnnotationBounds = bounds;
        viewport()->update();
        return;
    }

    if (m_movingTextAnnotation) {
        const QPointF point = pagePointFor(m_selectedTextAnnotationPage, pos);
        const QSizeF pageSize = m_pageSizes.at(m_selectedTextAnnotationPage);
        const QPointF delta = point - m_textAnnotationMoveStart;
        QRectF bounds = m_textAnnotationResizeOriginalBounds;
        const qreal x = qBound(0.0, bounds.left() + delta.x(),
                               qMax(0.0, pageSize.width() - bounds.width()));
        const qreal y = qBound(0.0, bounds.top() + delta.y(),
                               qMax(0.0, pageSize.height() - bounds.height()));
        bounds.moveTopLeft(QPointF(x, y));
        m_selectedTextAnnotationBounds = bounds;
        viewport()->update();
        return;
    }

    if (m_imageActive) {
        if (m_imageCropping) {
            if (m_imageCropDragging)
                updateImageCropRect(pos);
            else {
                const int handle = imageCropHandleAt(pos);
                if (handle >= 0)
                    viewport()->setCursor((handle == 0 || handle == 3)
                                              ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
                else if (imagePixelRect().contains(QPointF(pos)))
                    viewport()->setCursor(Qt::SizeAllCursor);
                else
                    viewport()->setCursor(Qt::ArrowCursor);
            }
            return;
        }
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

    if (!m_textTool && !m_drawTool && !m_imageAwaiting && !m_editor) {
        const int handle = textAnnotationResizeHandleAt(pos);
        if (handle >= 0) {
            viewport()->setCursor((handle == 0 || handle == 3)
                                      ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
        } else if (selectedTextAnnotationViewportBounds().contains(QPointF(pos))) {
            viewport()->setCursor(Qt::SizeAllCursor);
        } else {
            viewport()->setCursor(pageAtViewportPoint(pos) >= 0
                                      ? Qt::IBeamCursor : Qt::ArrowCursor);
        }
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_movingTextAnnotation && event->button() == Qt::LeftButton) {
        m_movingTextAnnotation = false;
        viewport()->setCursor(Qt::ArrowCursor);
        if (m_selectedTextAnnotationBounds != m_textAnnotationResizeOriginalBounds) {
            emit textAnnotationBoundsChanged(m_selectedTextAnnotationPage,
                                             m_selectedTextAnnotationId,
                                             m_selectedTextAnnotationContents,
                                             m_selectedTextAnnotationBounds);
        }
        return;
    }

    if (m_resizingTextAnnotation && event->button() == Qt::LeftButton) {
        m_resizingTextAnnotation = false;
        m_textAnnotationResizeCorner = -1;
        viewport()->setCursor(Qt::ArrowCursor);
        if (m_selectedTextAnnotationBounds != m_textAnnotationResizeOriginalBounds) {
            emit textAnnotationBoundsChanged(m_selectedTextAnnotationPage,
                                             m_selectedTextAnnotationId,
                                             m_selectedTextAnnotationContents,
                                             m_selectedTextAnnotationBounds);
        }
        return;
    }

    if (m_erasing && event->button() == Qt::LeftButton) {
        const int page = m_erasePage;
        const QVector<QPointF> path = m_erasePoints;
        const double radius = m_drawStyle.widthPoints / 2.0;
        m_erasing = false;
        m_erasePoints.clear();
        viewport()->update();
        if (!path.isEmpty())
            emit eraseRequested(page, path, radius);
        emit pendingChangesChanged();
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

    if (m_imageCropping && m_imageCropDragging && event->button() == Qt::LeftButton) {
        updateImageCropRect(event->position().toPoint());
        m_imageCropDragging = false;
        applyImageCrop();
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
        emit pendingChangesChanged();
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
    m_editorStyle = m_style;
    m_editingTextAnnotation = false;
    m_editingTextAnnotationId = 0;
    m_editingTextAnnotationOriginal.clear();

    m_editor = new QPlainTextEdit(viewport());
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->installEventFilter(this);
    connect(m_editor, &QPlainTextEdit::textChanged,
            this, &PageView::pendingChangesChanged);
    applyEditorStyle();
    positionEditor();
    m_editor->show();
    m_editor->setFocus();
    emit pendingChangesChanged();
}

void PageView::startAnnotationEditor(int page, int annotationId,
                                     const QRectF &boxPoints, const QString &text,
                                     const TextStyle &style)
{
    finishEditor(false);
    m_textTool = false;
    startEditor(page, boxPoints);
    m_editingTextAnnotation = true;
    m_editingTextAnnotationId = annotationId;
    m_editingTextAnnotationOriginal = text;
    m_editorStyle = style;
    m_editor->setPlainText(text);
    m_editor->moveCursor(QTextCursor::End);
    applyEditorStyle();
    m_editor->setFocus();
}

void PageView::applyEditorStyle()
{
    if (!m_editor)
        return;

    const QString background = m_editorStyle.color.lightness() > 200
        ? QStringLiteral("rgba(60,60,60,225)")
        : QStringLiteral("rgba(255,255,255,215)");

    m_editor->setStyleSheet(
        QStringLiteral("QPlainTextEdit { color: %1; background: %2; border: 2px solid #0066cc;"
                       " font-family: \"%3\"; font-size: %4px; }")
            .arg(m_editorStyle.color.name(), background, m_editorStyle.family)
            .arg(qMax(4, int(m_editorStyle.sizePoints * m_scale))));
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

    const QString text = editor->toPlainText();
    const int page = m_editorPage;
    const QRectF box = m_editorBox;
    const TextStyle style = m_editorStyle;
    const bool editingAnnotation = m_editingTextAnnotation;
    const int annotationId = m_editingTextAnnotationId;
    const QString originalText = m_editingTextAnnotationOriginal;
    m_editingTextAnnotation = false;
    m_editingTextAnnotationId = 0;
    m_editingTextAnnotationOriginal.clear();

    editor->hide();
    editor->deleteLater();

    if (!editingAnnotation) {
        m_textTool = false;
        viewport()->setCursor(Qt::ArrowCursor);
        setFocus();
        emit textToolFinished();
    }

    if (commit) {
        if (editingAnnotation) {
            if (text != originalText)
                emit textAnnotationEdited(page, annotationId, originalText, text);
        } else if (!text.trimmed().isEmpty()) {
            emit textPlaced(page, box, text.trimmed(), style);
        }
    }
    emit pendingChangesChanged();
}

void PageView::startFormEditor(int page, const FormField &field)
{
    finishFormEditor(false);
    m_activeFormField = field;
    m_formEditorPage = page;
    m_formEditorInitialValue = field.value;

    if (field.type == FormField::Type::Text && field.multiline) {
        m_formTextEditor = new QPlainTextEdit(viewport());
        m_formTextEditor->setPlainText(field.value);
        m_formTextEditor->installEventFilter(this);
        connect(m_formTextEditor, &QPlainTextEdit::textChanged,
                this, &PageView::pendingChangesChanged);
        m_formEditor = m_formTextEditor;
    } else if (field.type == FormField::Type::Text) {
        m_formLineEditor = new QLineEdit(viewport());
        m_formLineEditor->setText(field.value);
        m_formLineEditor->setEchoMode(field.password ? QLineEdit::Password
                                                     : QLineEdit::Normal);
        m_formLineEditor->installEventFilter(this);
        connect(m_formLineEditor, &QLineEdit::textChanged,
                this, &PageView::pendingChangesChanged);
        m_formEditor = m_formLineEditor;
    } else {
        m_formChoiceEditor = new QComboBox(viewport());
        m_formChoiceEditor->setEditable(field.editableChoice);
        for (int i = 0; i < field.options.size(); ++i) {
            const QString optionValue = i < field.optionValues.size()
                ? field.optionValues.at(i) : field.options.at(i);
            m_formChoiceEditor->addItem(field.options.at(i), optionValue);
        }
        int current = field.optionValues.indexOf(field.value);
        if (current < 0)
            current = field.options.indexOf(field.value);
        if (current < 0 && !field.editableChoice) {
            m_formChoiceEditor->insertItem(0, field.value, field.value);
            current = 0;
        }
        if (current >= 0) {
            m_formChoiceEditor->setCurrentIndex(current);
        } else if (field.editableChoice) {
            m_formChoiceEditor->setEditText(field.value);
        }
        m_formChoiceEditor->installEventFilter(this);
        connect(m_formChoiceEditor, &QComboBox::currentTextChanged,
                this, &PageView::pendingChangesChanged);
        if (m_formChoiceEditor->lineEdit())
            m_formChoiceEditor->lineEdit()->installEventFilter(this);
        m_formEditor = m_formChoiceEditor;
    }

    m_formEditor->setStyleSheet(
        QStringLiteral("QLineEdit, QPlainTextEdit, QComboBox {"
                       " color: #111; background: rgba(255,255,255,245);"
                       " border: 2px solid #0066cc; padding: 1px; }"));
    positionFormEditor();
    m_formEditor->show();
    m_formEditor->setFocus();
    if (m_formLineEditor)
        m_formLineEditor->selectAll();
    emit pendingChangesChanged();
}

void PageView::positionFormEditor()
{
    if (!m_formEditor || m_formEditorPage < 0 || m_formEditorPage >= m_pageRects.size())
        return;
    const QRect page = pageRectInViewport(m_formEditorPage);
    const QRect bounds(
        page.x() + qRound(m_activeFormField.bounds.x() * m_scale),
        page.y() + qRound(m_activeFormField.bounds.y() * m_scale),
        qMax(1, qRound(m_activeFormField.bounds.width() * m_scale)),
        qMax(1, qRound(m_activeFormField.bounds.height() * m_scale)));
    m_formEditor->setGeometry(bounds);
}

void PageView::finishFormEditor(bool commit)
{
    if (!m_formEditor)
        return;

    const int page = m_formEditorPage;
    const FormField field = m_activeFormField;
    const QString initialValue = m_formEditorInitialValue;
    const QString value = formEditorValue();

    QWidget *editor = m_formEditor;
    m_formEditor = nullptr;
    m_formLineEditor = nullptr;
    m_formTextEditor = nullptr;
    m_formChoiceEditor = nullptr;
    m_formEditorPage = -1;
    m_activeFormField = FormField();
    editor->hide();
    editor->deleteLater();
    setFocus();
    m_formEditorInitialValue.clear();

    if (commit && value != initialValue && m_formFieldValueHandler) {
        if (!m_formFieldValueHandler(page, field.widgetIndex, value)) {
            FormField retryField = field;
            retryField.value = value;
            if (page >= 0 && page < m_pageRects.size())
                startFormEditor(page, retryField);
            return;
        }

        FormFieldOverlay overlay;
        overlay.page = page;
        overlay.field = field;
        overlay.field.value = value;
        int existing = -1;
        for (int i = 0; i < m_formFieldOverlays.size(); ++i) {
            if (m_formFieldOverlays.at(i).page == page
                && m_formFieldOverlays.at(i).field.widgetIndex == field.widgetIndex) {
                existing = i;
                break;
            }
        }
        if (existing >= 0)
            m_formFieldOverlays[existing] = overlay;
        else
            m_formFieldOverlays.append(overlay);
        viewport()->update();
    }
    emit pendingChangesChanged();
}

QString PageView::formEditorValue() const
{
    if (m_formLineEditor)
        return m_formLineEditor->text();
    if (m_formTextEditor)
        return m_formTextEditor->toPlainText();
    if (!m_formChoiceEditor)
        return QString();

    const int matchingOption = m_formChoiceEditor->findText(
        m_formChoiceEditor->currentText());
    if (m_formChoiceEditor->isEditable() && matchingOption < 0)
        return m_formChoiceEditor->currentText();
    return m_formChoiceEditor->itemData(
        matchingOption >= 0 ? matchingOption
                            : m_formChoiceEditor->currentIndex()).toString();
}

bool PageView::eventFilter(QObject *watched, QEvent *event)
{
    if (m_formEditor
        && (watched == m_formEditor
            || (m_formChoiceEditor && watched == m_formChoiceEditor->lineEdit()))) {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                finishFormEditor(false);
                return true;
            }
            const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
            const bool multiline = m_formTextEditor != nullptr;
            if (enter && (!multiline || (key->modifiers() & Qt::ControlModifier))) {
                finishFormEditor(true);
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason) {
                QTimer::singleShot(0, this, [this]() {
                    if (!m_formEditor)
                        return;
                    QWidget *focus = QApplication::focusWidget();
                    if ((focus && (focus == m_formEditor || m_formEditor->isAncestorOf(focus)))
                        || QApplication::activePopupWidget())
                        return;
                    finishFormEditor(true);
                });
            }
        }
    }

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