#pragma once
#include <QDialog>

class QSpinBox;

class PageRangeDialog : public QDialog
{
public:
    PageRangeDialog(int pageCount, int currentPage, QWidget *parent = nullptr);

    int firstPage() const;   // 0-based, inclusive
    int lastPage() const;    // 0-based, inclusive

private:
    QSpinBox *m_from = nullptr;
    QSpinBox *m_to = nullptr;
};