#pragma once
#include <QAbstractScrollArea>
#include <QCache>
#include <QColor>
#include <QPixmap>
#include <QPointF>
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

class PageView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    enum class ZoomMode { Custom, FitToWidth, FitInView };

    struct TextStyle {
        QString pdfFont = QStringLiteral("Helv");
        double sizePoints = 12.0;
        QColor color = QColor(0, 0, 0);
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
    void commitPendingText();

signals:
    void currentPageChanged(int page);
    void textPlaced(int page, const QRectF &boxPoints, const QString &text,
                    const PageView::TextStyle &style);
    void textToolFinished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void onDocumentChanged();
    void recalculateLayout(bool keepPosition);
    void updateCurrentPage();
    int pageAtViewportPoint(const QPoint &point) const;
    QRect pageRectInViewport(int page) const;
    QPixmap pageImage(int page, const QSize &size, qreal dpr);
    void drawSearchResults(QPainter &painter, int page, const QRect &pageRect,
                           const QPdfLink &current);

    void startEditor(int page, const QRectF &boxPoints);
    void positionEditor();
    void applyEditorStyle();
    void finishEditor(bool commit);

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

    bool m_textTool = false;
    bool m_dragging = false;
    QPoint m_dragStart;
    QRect m_dragRect;
    QPlainTextEdit *m_editor = nullptr;
    int m_editorPage = -1;
    QRectF m_editorBox;
    TextStyle m_style;
};