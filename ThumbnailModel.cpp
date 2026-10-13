#include "ThumbnailModel.h"

#include <QPdfDocument>
#include <QPainter>

ThumbnailModel::ThumbnailModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_cache(300)   // keep at most 300 thumbnails
{
}

void ThumbnailModel::setDocument(QPdfDocument *document)
{
    m_document = document;
    if (!m_document)
        return;

    // Whenever the document loads, closes, or is replaced:
    // throw away old thumbnails and tell the view to start over.
    connect(m_document, &QPdfDocument::statusChanged, this, [this]() {
        beginResetModel();
        m_cache.clear();
        endResetModel();
    });
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

        const QPixmap pixmap = renderThumbnail(page);
        m_cache.insert(page, new QPixmap(pixmap));   // cache takes ownership
        return pixmap;
    }

    return {};
}

QPixmap ThumbnailModel::renderThumbnail(int page) const
{
    // Scale the page to fit inside the thumbnail box, keeping its shape.
    const QSizeF pts = m_document->pagePointSize(page);
    const qreal scale = qMin(kThumbWidth / pts.width(), kThumbHeight / pts.height());
    const QSize size(qMax(1, int(pts.width() * scale)),
                     qMax(1, int(pts.height() * scale)));

    const QImage rendered = m_document->render(page, size);

    // Draw onto a white canvas so transparent areas look like paper.
    QImage canvas(rendered.size(), QImage::Format_RGB32);
    canvas.fill(Qt::white);

    QPainter painter(&canvas);
    painter.drawImage(0, 0, rendered);
    painter.end();

    return QPixmap::fromImage(canvas);
}