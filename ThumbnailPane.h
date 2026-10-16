#pragma once
#include <QListView>

class QDropEvent;
class QPdfDocument;
class ThumbnailModel;

class ThumbnailPane : public QListView
{
    Q_OBJECT
public:
    explicit ThumbnailPane(QWidget *parent = nullptr);

    void setDocument(QPdfDocument *document);
    void setCurrentPage(int page);        // highlight a page without emitting a signal
    void setReorderEnabled(bool enabled); // allow drag-and-drop reordering

signals:
    void pageRequested(int page);                  // user clicked a thumbnail
    void pageMoveRequested(int from, int to);      // user dragged a thumbnail

protected:
    void dropEvent(QDropEvent *event) override;

private:
    ThumbnailModel *m_model = nullptr;
    bool m_syncing = false;
};