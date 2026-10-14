#pragma once
#include <QAbstractListModel>
#include <QCache>
#include <QPixmap>

class QPdfDocument;

class ThumbnailModel : public QAbstractListModel
{
    Q_OBJECT
public:
    static constexpr int kThumbWidth = 120;
    static constexpr int kThumbHeight = 170;

    explicit ThumbnailModel(QObject *parent = nullptr);

    void setDocument(QPdfDocument *document);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

private:
    QPixmap renderThumbnail(int page) const;

    QPdfDocument *m_document = nullptr;
    mutable QCache<int, QPixmap> m_cache;
};