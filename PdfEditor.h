#pragma once
#include <QString>

struct fz_context;
struct pdf_document;

class PdfEditor
{
public:
    PdfEditor();
    ~PdfEditor();

    PdfEditor(const PdfEditor &) = delete;            // owns raw pointers: no copying
    PdfEditor &operator=(const PdfEditor &) = delete;

    bool open(const QString &path, const QString &password = QString());
    bool saveAs(const QString &path);
    bool rotatePage(int pageIndex, int deltaDegrees);
    bool movePage(int fromIndex, int toIndex);
    bool deletePage(int pageIndex);
    void close();

    QString lastError() const { return m_error; }

private:
    fz_context *m_ctx = nullptr;
    pdf_document *m_doc = nullptr;
    QString m_error;
};