#pragma once
#include <QListView>

class QPdfDocument;
class ThumbnailModel;

class ThumbnailPane : public QListView
{
    Q_OBJECT
public:
    explicit ThumbnailPane(QWidget *parent = nullptr);

    void setDocument(QPdfDocument *document);
    void setCurrentPage(int page);

signals:
    void pageRequested(int page);

private:
    ThumbnailModel *m_model = nullptr;
    bool m_syncing = false;
};