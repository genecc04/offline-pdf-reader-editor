#include "SearchPane.h"

#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <QPdfDocument>
#include <QPdfLink>
#include <QPdfSearchModel>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {

    class ResultDelegate : public QStyledItemDelegate
    {
    public:
        ResultDelegate(const QPdfSearchModel *model, QObject *parent)
            : QStyledItemDelegate(parent), m_model(model) {}

    protected:
        void initStyleOption(QStyleOptionViewItem *option,
                            const QModelIndex &index) const override
        {
            QStyledItemDelegate::initStyleOption(option, index);

            const int page = index.data(int(QPdfSearchModel::Role::Page)).toInt();
            const QString before = index.data(int(QPdfSearchModel::Role::ContextBefore)).toString();
            const QString after = index.data(int(QPdfSearchModel::Role::ContextAfter)).toString();

            option->text = QString("p.%1   ...%2[%3]%4...")
                            .arg(page + 1)
                            .arg(before, m_model->searchString(), after);
        }

    private:
        const QPdfSearchModel *m_model;
    };

}

SearchPane::SearchPane(QWidget *parent)
    : QWidget(parent)
{
    m_model = new QPdfSearchModel(this);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText("Search in document...");
    m_input->setClearButtonEnabled(true);

    m_results = new QListView(this);
    m_results->setModel(m_model);
    m_results->setItemDelegate(new ResultDelegate(m_model, m_results));
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_input);
    layout->addWidget(m_results);

    // Search when Enter is pressed; clear results when the box is emptied.
    connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        m_model->setSearchString(m_input->text());
    });
    connect(m_input, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.isEmpty())
            m_model->setSearchString(QString());
    });

    // Selecting a result tells the outside world where it is.
    connect(m_results->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current) {
                if (!current.isValid())
                    return;
                const QPdfLink link = m_model->resultAtIndex(current.row());
                emit resultSelected(current.row(), link.page(), link.location());
            });
}

void SearchPane::setDocument(QPdfDocument *document)
{
    m_model->setDocument(document);
}

void SearchPane::focusSearchBox()
{
    m_input->setFocus();
    m_input->selectAll();
}