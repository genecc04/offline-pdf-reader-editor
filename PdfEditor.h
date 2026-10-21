#pragma once
#include <QString>
#include <QRectF>
#include <QPointF>
#include <QVector>
#include <QByteArray>

struct fz_context;
struct pdf_document;

class PdfEditor
{
public:
    PdfEditor();
    ~PdfEditor();

    PdfEditor(const PdfEditor &) = delete;
    PdfEditor &operator=(const PdfEditor &) = delete;
    bool addTextBox(int pageIndex, const QRectF &box, const QString &text,
                const QString &fontName, double fontSize,
                float red, float green, float blue);

    bool open(const QString &path, const QString &password = QString());
    enum class Encryption { Keep, Remove, Aes256 };

    bool saveAs(const QString &path, bool removeUnused = false,
                Encryption encryption = Encryption::Keep,
                const QString &password = QString());
    bool keepOnlyPages(int firstIndex, int lastIndex);
    bool rotatePage(int pageIndex, int deltaDegrees);
    bool movePage(int fromIndex, int toIndex);
    bool insertPagesFrom(const QString &sourcePath, int insertAt);
    bool deletePage(int pageIndex);
    bool highlightText(const QString &text, int *pagesChanged = nullptr);
    bool bakeAnnotations();
    bool hasAnnotations();
    bool addInkStroke(int pageIndex, const QVector<QPointF> &points,
                    float red, float green, float blue, double width, double opacity);
    bool addImageStamp(int pageIndex, const QRectF &box, const QByteArray &imageData);
    void close();

    QString lastError() const { return m_error; }

private:
    fz_context *m_ctx = nullptr;
    pdf_document *m_doc = nullptr;
    QString m_error;
};