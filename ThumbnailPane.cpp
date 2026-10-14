#include "ThumbnailPane.h"
#include "ThumbnailModel.h"

#include <QItemSelectionModel>

ThumbnailPane::ThumbnailPane(QWidget *parent)
    : QListView(parent)
{
    m_model = new ThumbnailModel(this);
    setModel(m_model);

    setViewMode(QListView::IconMode);
    setFlow(QListView::TopToBottom);
    setWrapping(false);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setIconSize(QSize(ThumbnailModel::kThumbWidth, ThumbnailModel::kThumbHeight));
    setSpacing(8);
    setUniformItemSizes(true);

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

    m_syncing = true;
    setCurrentIndex(index);
    m_syncing = false;
}