#pragma once
#include <QPointF>
#include <QTreeView>

class QPdfBookmarkModel;
class QPdfDocument;

class BookmarkPane : public QTreeView
{
    Q_OBJECT
public:
    explicit BookmarkPane(QWidget *parent = nullptr);

    void setDocument(QPdfDocument *document);

signals:
    void bookmarkSelected(int page, QPointF location);

private:
    QPdfBookmarkModel *m_model = nullptr;
};