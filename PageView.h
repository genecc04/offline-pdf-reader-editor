#pragma once
#include <QAbstractScrollArea>
#include <QCache>
#include <QColor>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QVector>

class QPdfDocument;
class QPdfLink;
class QPdfSearchModel;
class QPlainTextEdit;
class QTimer;

class PageView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    enum class ZoomMode { Custom, FitToWidth, FitInView };

    struct TextStyle {
        QString family = QStringLiteral("Arial");
        bool bold = false;
        bool italic = false;
        double sizePoints = 12.0;
        QColor color = QColor(0, 0, 0);
    };

    struct DrawStyle {
        QColor color = QColor(0, 0, 0);
        double widthPoints = 2.0;
        double opacity = 1.0;
    };

    struct Stroke {
        int page = -1;
        QVector<QPointF> points;
        DrawStyle style;
    };

    explicit PageView(QWidget *parent = nullptr);

    void setDocument(QPdfDocument *document);
    void setSearchModel(QPdfSearchModel *model);
    void setCurrentSearchResultIndex(int index);

    int currentPage() const { return m_currentPage; }
    void jumpToPage(int page, const QPointF &location = QPointF());

    void setZoomMode(ZoomMode mode);
    ZoomMode zoomMode() const { return m_zoomMode; }
    void setZoomFactor(double factor);
    double zoomFactor() const;

    void setTextToolActive(bool active);
    void setTextStyle(const TextStyle &style);

    void setDrawToolActive(bool active);
    void setDrawStyle(const DrawStyle &style);
    void setEraserActive(bool active);
    bool hasPendingStrokes() const { return !m_strokes.isEmpty(); }
    bool undoLastStroke();

    void beginImagePlacement(const QImage &image);

    void commitPendingEdits();

    bool hasTextSelection() const { return !m_selection.isEmpty(); }
    void copySelection();
    void selectAllText();
    void clearSelection();

signals:
    void currentPageChanged(int page);
    void textPlaced(int page, const QRectF &boxPoints, const QString &text,
                    const PageView::TextStyle &style);
    void textToolFinished();
    void strokesPlaced(const QVector<PageView::Stroke> &strokes);
    void eraseRequested(int page, const QVector<QPointF> &path, double radius);
    void pendingStrokesChanged();
    void imagePlaced(int page, const QRectF &boxPoints, const QImage &image);
    void textSelected();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class ImageDrag { None, Move, Resize };

    struct SelectionPart {
        int page = -1;
        QList<QPolygonF> bounds;
        QString text;
    };

    void onDocumentChanged();
    void recalculateLayout(bool keepPosition);
    void updateCurrentPage();
    int pageAtViewportPoint(const QPoint &point) const;
    int nearestPage(const QPoint &viewportPos) const;
    QRect pageRectInViewport(int page) const;
    QPointF pagePointFor(int page, const QPoint &viewportPos) const;
    QPixmap pageImage(int page, const QSize &size, qreal dpr);
    void drawSearchResults(QPainter &painter, int page, const QRect &pageRect,
                           const QPdfLink &current);
    void drawStroke(QPainter &painter, const Stroke &stroke, const QRect &pageRect);

    void startEditor(int page, const QRectF &boxPoints);
    void positionEditor();
    void applyEditorStyle();
    void finishEditor(bool commit);
    void commitStrokes();

    void finishImage(bool commit);
    void drawImagePlacement(QPainter &painter, const QRect &pageRect);
    QRectF imagePixelRect() const;
    int imageHandleAt(const QPoint &viewportPos) const;
    void updateImageCursor(const QPoint &viewportPos);

    void updateSelection();
    void extendSelection(const QPoint &viewportPos);

    QPdfDocument *m_document = nullptr;
    QPdfSearchModel *m_search = nullptr;
    int m_searchIndex = -1;

    ZoomMode m_zoomMode = ZoomMode::FitToWidth;
    double m_customZoom = 1.0;
    double m_scale = 1.0;
    QVector<QSizeF> m_pageSizes;
    QVector<QRect> m_pageRects;
    QSize m_documentSize;
    int m_currentPage = 0;
    bool m_jumping = false;

    QCache<quint64, QPixmap> m_cache;

    // text tool
    bool m_textTool = false;
    bool m_dragging = false;
    QPoint m_dragStart;
    QRect m_dragRect;
    QPlainTextEdit *m_editor = nullptr;
    int m_editorPage = -1;
    QRectF m_editorBox;
    TextStyle m_style;

    // draw tool
    bool m_drawTool = false;
    bool m_drawing = false;
    bool m_eraserActive = false;
    bool m_erasing = false;
    int m_erasePage = -1;
    QPoint m_eraserCursorPos;
    QVector<QPointF> m_erasePoints;
    DrawStyle m_drawStyle;
    Stroke m_activeStroke;
    QVector<Stroke> m_strokes;

    // image placement
    QImage m_pendingImage;
    bool m_imageAwaiting = false;
    bool m_imageActive = false;
    int m_imagePage = -1;
    QRectF m_imageRect;
    ImageDrag m_imageDrag = ImageDrag::None;
    int m_imageHandle = -1;
    QPointF m_imageGrab;
    QPointF m_imageAnchor;

    // text selection
    bool m_selecting = false;
    bool m_selMoved = false;
    QPoint m_selPressPos;
    QPoint m_lastMouse;
    int m_selStartPage = -1;
    QPointF m_selStartPoint;
    int m_selEndPage = -1;
    QPointF m_selEndPoint;
    QVector<SelectionPart> m_selection;
    QTimer *m_autoScroll = nullptr;
};