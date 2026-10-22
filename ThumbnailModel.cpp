#include "ThumbnailModel.h"

#include <QPdfDocument>
#include <QPainter>
#include <QMimeData>
#include <QPdfDocumentRenderOptions>
#include <QTimer>

ThumbnailModel::ThumbnailModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_cache(300)
{
    m_renderTimer = new QTimer(this);
    m_renderTimer->setSingleShot(true);
    m_renderTimer->setInterval(5);
    connect(m_renderTimer, &QTimer::timeout, this, [this]() {
        if (m_renderQueue.isEmpty())
            return;

        const int page = m_renderQueue.dequeue();
        m_queuedPages.remove(page);
        QPdfDocument *source = m_pageRenderDocument
                                       && m_pageRenderDocument->status()
                                              == QPdfDocument::Status::Ready
                                   ? m_pageRenderDocument
                                   : m_document;
        if (source && source->status() == QPdfDocument::Status::Ready
            && page >= 0 && page < source->pageCount()
            && !m_cache.object(page)) {
            const QPixmap thumbnail = renderThumbnail(page);
            m_cache.insert(page, new QPixmap(thumbnail));
            const QModelIndex pageIndex = index(page);
            emit dataChanged(pageIndex, pageIndex, { Qt::DecorationRole });
        }

        if (!m_renderQueue.isEmpty())
            m_renderTimer->start();
    });
}

void ThumbnailModel::setDocument(QPdfDocument *document)
{
    m_document = document;
    if (!m_document) {
        m_cache.clear();
        m_renderQueue.clear();
        m_queuedPages.clear();
        m_renderTimer->stop();
        return;
    }

    connect(m_document, &QPdfDocument::statusChanged, this, [this]() {
        beginResetModel();
        m_cache.clear();
        m_renderQueue.clear();
        m_queuedPages.clear();
        m_renderTimer->stop();
        endResetModel();
    });
}

void ThumbnailModel::setPageRenderDocument(QPdfDocument *document,
                                           const QList<int> &changedPages)
{
    m_pageRenderDocument = document;
    if (changedPages.isEmpty()) {
        m_cache.clear();
        m_renderQueue.clear();
        m_queuedPages.clear();
        m_renderTimer->stop();
        return;
    }

    for (int page : changedPages) {
        if (page < 0 || page >= rowCount())
            continue;
        m_cache.remove(page);
        emit dataChanged(index(page), index(page), { Qt::DecorationRole });
    }
}

int ThumbnailModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_document
        || m_document->status() != QPdfDocument::Status::Ready)
        return 0;
    return m_document->pageCount();
}

QVariant ThumbnailModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};

    const int page = index.row();

    if (role == Qt::DisplayRole)
        return QString::number(page + 1);

    if (role == Qt::DecorationRole) {
        if (QPixmap *cached = m_cache.object(page))
            return *cached;

        if (!m_queuedPages.contains(page)) {
            m_queuedPages.insert(page);
            m_renderQueue.enqueue(page);
            if (!m_renderTimer->isActive())
                m_renderTimer->start();
        }

        static const QPixmap placeholder = []() {
            QPixmap pixmap(kThumbWidth, kThumbHeight);
            pixmap.fill(Qt::white);
            QPainter painter(&pixmap);
            painter.setPen(QColor(210, 210, 210));
            painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
            return pixmap;
        }();
        return placeholder;
    }

    return {};
}

QPixmap ThumbnailModel::renderThumbnail(int page) const
{
    QPdfDocument *source = m_pageRenderDocument
                                   && m_pageRenderDocument->status() == QPdfDocument::Status::Ready
                               ? m_pageRenderDocument
                               : m_document;
    const QSizeF pts = source->pagePointSize(page);
    const qreal scale = qMin(kThumbWidth / pts.width(), kThumbHeight / pts.height());
    const QSize size(qMax(1, int(pts.width() * scale)),
                     qMax(1, int(pts.height() * scale)));

    QPdfDocumentRenderOptions options;
    options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
    const QImage rendered = source->render(page, size, options);

    QImage canvas(rendered.size(), QImage::Format_RGB32);
    canvas.fill(Qt::white);

    QPainter painter(&canvas);
    painter.drawImage(0, 0, rendered);
    painter.end();

    return QPixmap::fromImage(canvas);
}

Qt::ItemFlags ThumbnailModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;

    return QAbstractListModel::flags(index) | Qt::ItemIsDragEnabled;
}

Qt::DropActions ThumbnailModel::supportedDragActions() const
{
    return Qt::MoveAction;
}

Qt::DropActions ThumbnailModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

QStringList ThumbnailModel::mimeTypes() const
{
    return { kPageMimeType };
}

QMimeData *ThumbnailModel::mimeData(const QModelIndexList &indexes) const
{
    auto *mime = new QMimeData;
    if (!indexes.isEmpty())
        mime->setData(kPageMimeType, QByteArray::number(indexes.first().row()));
    return mime;
}