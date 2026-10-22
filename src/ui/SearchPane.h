#pragma once
#include <QPointF>
#include <QWidget>

class QLineEdit;
class QListView;
class QPdfDocument;
class QPdfSearchModel;

class SearchPane : public QWidget
{
    Q_OBJECT
public:
    explicit SearchPane(QWidget *parent = nullptr);

    void setDocument(QPdfDocument *document);
    QPdfSearchModel *searchModel() const { return m_model; }
    void focusSearchBox();

signals:
    void resultSelected(int index, int page, QPointF location);

private:
    QLineEdit *m_input = nullptr;
    QListView *m_results = nullptr;
    QPdfSearchModel *m_model = nullptr;
};