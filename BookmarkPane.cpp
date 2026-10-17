#include "BookmarkPane.h"

#include <QPdfBookmarkModel>
#include <QPdfDocument>

BookmarkPane::BookmarkPane(QWidget *parent)
    : QTreeView(parent)
{
    m_model = new QPdfBookmarkModel(this);
    setModel(m_model);
    setHeaderHidden(true);

    connect(this, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        const int page = index.data(int(QPdfBookmarkModel::Role::Page)).toInt();
        const QPointF location = index.data(int(QPdfBookmarkModel::Role::Location)).toPointF();
        emit bookmarkSelected(page, location);
    });
}

void BookmarkPane::setDocument(QPdfDocument *document)
{
    m_model->setDocument(document);
}