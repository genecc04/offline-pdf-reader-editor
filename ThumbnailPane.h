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
    void setCurrentPage(int page);
    void setReorderEnabled(bool enabled);

signals:
    void pageRequested(int page);
    void pageMoveRequested(int from, int to);

protected:
    void dropEvent(QDropEvent *event) override;

private:
    ThumbnailModel *m_model = nullptr;
    bool m_syncing = false;
};