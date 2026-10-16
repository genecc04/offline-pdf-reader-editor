#include "ThumbnailPane.h"
#include "ThumbnailModel.h"

#include <QDropEvent>
#include <QItemSelectionModel>
#include <QMimeData>

ThumbnailPane::ThumbnailPane(QWidget *parent)
    : QListView(parent)
{
    m_model = new ThumbnailModel(this);
    setModel(m_model);

    // A plain vertical list: thumbnail with its page number beside it.
    setViewMode(QListView::ListMode);
    setIconSize(QSize(ThumbnailModel::kThumbWidth, ThumbnailModel::kThumbHeight));
    setSpacing(8);
    setUniformItemSizes(true);

    // Reordering stays off until the main window says editing is possible.
    setDragDropMode(QAbstractItemView::NoDragDrop);
    setDropIndicatorShown(true);
    setDefaultDropAction(Qt::MoveAction);

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

void ThumbnailPane::setReorderEnabled(bool enabled)
{
    setDragDropMode(enabled ? QAbstractItemView::InternalMove
                            : QAbstractItemView::NoDragDrop);
}

void ThumbnailPane::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasFormat(ThumbnailModel::kPageMimeType)) {
        event->ignore();
        return;
    }

    const int from = event->mimeData()->data(ThumbnailModel::kPageMimeType).toInt();
    const int count = m_model->rowCount();
    const int y = event->position().toPoint().y();

    // Insert before the first thumbnail whose middle is below the cursor.
    int insertBefore = count;
    for (int row = 0; row < count; ++row) {
        if (y < visualRect(m_model->index(row)).center().y()) {
            insertBefore = row;
            break;
        }
    }

    // Removing the dragged page first shifts everything after it up by one.
    const int to = (insertBefore > from) ? insertBefore - 1 : insertBefore;

    event->acceptProposedAction();   // we handle the move ourselves; the model stays read-only
    if (to != from)
        emit pageMoveRequested(from, to);
}