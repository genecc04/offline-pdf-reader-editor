#pragma once
#include <QString>

struct fz_context;
struct pdf_document;

class PdfEditor
{
public:
    PdfEditor();
    ~PdfEditor();

    PdfEditor(const PdfEditor &) = delete;
    PdfEditor &operator=(const PdfEditor &) = delete;

    bool open(const QString &path, const QString &password = QString());
    bool saveAs(const QString &path, bool removeUnused = false);
    bool keepOnlyPages(int firstIndex, int lastIndex);
    bool rotatePage(int pageIndex, int deltaDegrees);
    bool movePage(int fromIndex, int toIndex);
    bool insertPagesFrom(const QString &sourcePath, int insertAt);
    bool deletePage(int pageIndex);
    bool highlightText(const QString &text, int *pagesChanged = nullptr);
    bool bakeAnnotations();
    bool hasAnnotations();
    void close();

    QString lastError() const { return m_error; }

private:
    fz_context *m_ctx = nullptr;
    pdf_document *m_doc = nullptr;
    QString m_error;
};