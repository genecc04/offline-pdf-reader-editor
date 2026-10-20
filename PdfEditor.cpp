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

bool PdfEditor::insertPagesFrom(const QString &sourcePath, int insertAt)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    const QByteArray pathUtf8 = sourcePath.toUtf8();
    const char *cPath = pathUtf8.constData();

    pdf_document *src = nullptr;
    pdf_graft_map *map = nullptr;
    fz_var(src);
    fz_var(map);

    fz_try(m_ctx)
    {
        src = pdf_open_document(m_ctx, cPath);
        if (fz_needs_password(m_ctx, &src->super))
            fz_throw(m_ctx, FZ_ERROR_GENERIC,
                     "The PDF to insert is password-protected, which is not supported yet.");

        const int destCount = pdf_count_pages(m_ctx, m_doc);
        const int srcCount = pdf_count_pages(m_ctx, src);
        if (insertAt < 0 || insertAt > destCount)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "That position is not valid.");

        map = pdf_new_graft_map(m_ctx, m_doc);
        for (int i = 0; i < srcCount; ++i)
            pdf_graft_mapped_page(m_ctx, map, insertAt + i, src, i);
    }
    fz_always(m_ctx)
    {
        if (map)
            pdf_drop_graft_map(m_ctx, map);
        if (src)
            pdf_drop_document(m_ctx, src);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::highlightText(const QString &text, int *pagesChanged)
{
    if (pagesChanged)
        *pagesChanged = 0;
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (text.trimmed().isEmpty()) {
        m_error = "Nothing to search for.";
        return false;
    }

    const QByteArray needleUtf8 = text.toUtf8();
    const char *needle = needleUtf8.constData();

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

    constexpr int kMaxHits = 512;
    std::vector<fz_quad> quads(kMaxHits);
    std::vector<int> marks(kMaxHits);
    int changed = 0;

    for (int pageIndex = 0; pageIndex < count; ++pageIndex) {
        pdf_page *page = nullptr;
        int hits = 0;
        fz_var(page);
        fz_var(hits);

        fz_try(m_ctx)
        {
            page = pdf_load_page(m_ctx, m_doc, pageIndex);
            hits = fz_search_page(m_ctx, &page->super, needle,
                                  marks.data(), quads.data(), kMaxHits);
            if (hits > 0) {
                // One yellow highlight per page, covering every match on it.
                pdf_annot *annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_HIGHLIGHT);
                const float yellow[3] = { 1.0f, 0.92f, 0.0f };
                pdf_set_annot_color(m_ctx, annot, 3, yellow);
                pdf_set_annot_quad_points(m_ctx, annot, hits, quads.data());
                pdf_update_annot(m_ctx, annot);
            }
        }
        fz_always(m_ctx)
        {
            if (page)
                fz_drop_page(m_ctx, &page->super);
        }
        fz_catch(m_ctx)
        {
            m_error = QString::fromUtf8(fz_caught_message(m_ctx));
            return false;
        }

        if (hits > 0)
            ++changed;
    }

    if (pagesChanged)
        *pagesChanged = changed;
    return true;
}

bool PdfEditor::bakeAnnotations()
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    fz_try(m_ctx)
    {
        // Draw annotations into the page content (form fields are left alone).
        pdf_bake_document(m_ctx, m_doc, 1, 0);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}

bool PdfEditor::hasAnnotations()
{
    if (!m_doc)
        return false;

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

    for (int i = 0; i < count; ++i) {
        pdf_page *page = nullptr;
        bool found = false;
        fz_var(page);
        fz_var(found);

        fz_try(m_ctx)
        {
            page = pdf_load_page(m_ctx, m_doc, i);
            found = (pdf_first_annot(m_ctx, page) != nullptr);
        }
        fz_always(m_ctx)
        {
            if (page)
                fz_drop_page(m_ctx, &page->super);
        }
        fz_catch(m_ctx)
        {
            m_error = QString::fromUtf8(fz_caught_message(m_ctx));
            return false;
        }

        if (found)
            return true;
    }
    return false;
}

bool PdfEditor::addTextBox(int pageIndex, const QRectF &box, const QString &text,
                           const QString &fontName, double fontSize,
                           float red, float green, float blue)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    const QByteArray textUtf8 = text.toUtf8();
    const QByteArray fontUtf8 = fontName.toUtf8();
    const char *cText = textUtf8.constData();
    const char *cFont = fontUtf8.constData();
    const float color[3] = { red, green, blue };
    const fz_rect rect = fz_make_rect(float(box.left()), float(box.top()),
                                      float(box.right()), float(box.bottom()));

    pdf_page *page = nullptr;
    fz_var(page);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);

        pdf_annot *annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_FREE_TEXT);
        pdf_set_annot_rect(m_ctx, annot, rect);
        pdf_set_annot_default_appearance(m_ctx, annot, cFont, float(fontSize), 3, color);
        pdf_set_annot_contents(m_ctx, annot, cText);
        pdf_update_annot(m_ctx, annot);
    }
    fz_always(m_ctx)
    {
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    return true;
}