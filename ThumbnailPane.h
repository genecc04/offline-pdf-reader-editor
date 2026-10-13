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
    void setCurrentPage(int page);   // highlight a page without emitting a signal

signals:
    void pageRequested(int page);    // user clicked a thumbnail

private:
    ThumbnailModel *m_model = nullptr;
    bool m_syncing = false;
};