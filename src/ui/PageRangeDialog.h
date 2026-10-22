#pragma once
#include <QDialog>

class QSpinBox;

class PageRangeDialog : public QDialog
{
public:
    PageRangeDialog(int pageCount, int currentPage, QWidget *parent = nullptr);

    int firstPage() const;
    int lastPage() const;

private:
    QSpinBox *m_from = nullptr;
    QSpinBox *m_to = nullptr;
};