#include "ThumbnailPane.h"
#include "ThumbnailModel.h"

#include <QItemSelectionModel>

ThumbnailPane::ThumbnailPane(QWidget *parent)
    : QListView(parent)
{
    m_model = new ThumbnailModel(this);
    setModel(m_model);

    setViewMode(QListView::IconMode);
    setFlow(QListView::TopToBottom);   // one vertical column
    setWrapping(false);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setIconSize(QSize(ThumbnailModel::kThumbWidth, ThumbnailModel::kThumbHeight));
    setSpacing(8);
    setUniformItemSizes(true);         // lets the view lay out without asking every row

    connect(selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex &current) {
                if (m_syncing || !current.isValid())
                    return;
                emit pageRequested(current.row());
            });
}

void ThumbnailPane::setDocument(QPdfDocument *document)
{
    m_model->setDocument(document);
}

void ThumbnailPane::setCurrentPage(int page)
{
    const QModelIndex index = m_model->index(page);
    if (!index.isValid())
        return;

    m_syncing = true;               // so our own change doesn't echo as a click
    setCurrentIndex(index);
    m_syncing = false;
}