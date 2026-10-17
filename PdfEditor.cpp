#include "PdfEditor.h"

#include <QByteArray>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

PdfEditor::PdfEditor()
{
    m_ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
}

PdfEditor::~PdfEditor()
{
    close();
    if (m_ctx)
        fz_drop_context(m_ctx);
}

void PdfEditor::close()
{
    if (m_doc) {
        pdf_drop_document(m_ctx, m_doc);
        m_doc = nullptr;
    }
}

bool PdfEditor::open(const QString &path, const QString &password)
{
    close();
    if (!m_ctx) {
        m_error = "Could not start the PDF engine.";
        return false;
    }

    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray passUtf8 = password.toUtf8();
    const char *cPath = pathUtf8.constData();
    const char *cPass = passUtf8.constData();

    pdf_document *doc = nullptr;
    fz_var(doc);

    fz_try(m_ctx)
    {
        doc = pdf_open_document(m_ctx, cPath);
        if (fz_needs_password(m_ctx, &doc->super)
            && !fz_authenticate_password(m_ctx, &doc->super, cPass))
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "Incorrect or missing password");
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        pdf_drop_document(m_ctx, doc);
        return false;
    }

    m_doc = doc;
    return true;
}

bool PdfEditor::saveAs(const QString &path, bool removeUnused)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    const QByteArray pathUtf8 = path.toUtf8();
    const char *cPath = pathUtf8.constData();
    pdf_write_options opts = pdf_default_write_options;
    if (removeUnused)
        opts.do_garbage = 2;

    fz_try(m_ctx)
    {
        pdf_save_document(m_ctx, m_doc, cPath, &opts);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::rotatePage(int pageIndex, int deltaDegrees)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    fz_try(m_ctx)
    {
        pdf_obj *pageObj = pdf_lookup_page_obj(m_ctx, m_doc, pageIndex);

        const int current = pdf_to_int(m_ctx,
            pdf_dict_get_inheritable(m_ctx, pageObj, PDF_NAME(Rotate)));

        const int updated = ((current + deltaDegrees) % 360 + 360) % 360;

        pdf_dict_put_int(m_ctx, pageObj, PDF_NAME(Rotate), updated);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::deletePage(int pageIndex)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    fz_try(m_ctx)
    {
        const int count = pdf_count_pages(m_ctx, m_doc);
        if (pageIndex < 0 || pageIndex >= count)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "That page does not exist.");
        if (count <= 1)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "A PDF must keep at least one page.");

        pdf_delete_page(m_ctx, m_doc, pageIndex);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::movePage(int fromIndex, int toIndex)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    int count = 0;
    fz_var(count);
    fz_try(m_ctx)
    {
        count = pdf_count_pages(m_ctx, m_doc);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    if (fromIndex < 0 || fromIndex >= count || toIndex < 0 || toIndex >= count) {
        m_error = "That page does not exist.";
        return false;
    }
    if (fromIndex == toIndex)
        return true;

    std::vector<int> order;
    for (int i = 0; i < count; ++i) {
        if (i != fromIndex)
            order.push_back(i);
    }
    order.insert(order.begin() + toIndex, fromIndex);
    const int *pages = order.data();

    fz_try(m_ctx)
    {
        pdf_rearrange_pages(m_ctx, m_doc, count, pages, PDF_CLEAN_STRUCTURE_DROP);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::keepOnlyPages(int firstIndex, int lastIndex)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    int count = 0;
    fz_var(count);
    fz_try(m_ctx)
    {
        count = pdf_count_pages(m_ctx, m_doc);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    if (firstIndex < 0 || lastIndex >= count || firstIndex > lastIndex) {
        m_error = "That page range is not valid.";
        return false;
    }

    // The pages to keep, in order. Built outside fz_try, as before.
    std::vector<int> keep;
    for (int i = firstIndex; i <= lastIndex; ++i)
        keep.push_back(i);
    const int *pages = keep.data();
    const int keepCount = int(keep.size());

    fz_try(m_ctx)
    {
        pdf_rearrange_pages(m_ctx, m_doc, keepCount, pages, PDF_CLEAN_STRUCTURE_DROP);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}