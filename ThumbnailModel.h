#pragma once
#include <QAbstractListModel>
#include <QCache>
#include <QPixmap>

class QMimeData;
class QPdfDocument;

class ThumbnailModel : public QAbstractListModel
{
    Q_OBJECT
public:
    static constexpr int kThumbWidth = 120;
    static constexpr int kThumbHeight = 170;
    static inline const QString kPageMimeType = QStringLiteral("application/x-pdfapp-page");

    explicit ThumbnailModel(QObject *parent = nullptr);

    void setDocument(QPdfDocument *document);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    // Drag and drop (used for reordering pages)
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    Qt::DropActions supportedDragActions() const override;
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

private:
    QPixmap renderThumbnail(int page) const;

    QPdfDocument *m_document = nullptr;
    mutable QCache<int, QPixmap> m_cache;
};