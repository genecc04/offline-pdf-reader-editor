#include "PdfEditor.h"

#include <QByteArray>
#include <vector>
#include <cstring>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace {

double pointSegmentDistanceSquared(const QPointF &point, const QPointF &start,
                                   const QPointF &end)
{
    const double dx = end.x() - start.x();
    const double dy = end.y() - start.y();
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared == 0.0) {
        const double px = point.x() - start.x();
        const double py = point.y() - start.y();
        return px * px + py * py;
    }

    const double projection = qBound(
        0.0, ((point.x() - start.x()) * dx + (point.y() - start.y()) * dy)
                 / lengthSquared,
        1.0);
    const double px = point.x() - (start.x() + projection * dx);
    const double py = point.y() - (start.y() + projection * dy);
    return px * px + py * py;
}

struct LineSegment {
    QPointF start;
    QPointF end;
    QRectF bounds;
};

QRectF segmentBounds(const QPointF &start, const QPointF &end)
{
    return QRectF(start, end).normalized();
}

bool boundsIntersect(const QRectF &a, const QRectF &b)
{
    return a.left() <= b.right() && a.right() >= b.left()
           && a.top() <= b.bottom() && a.bottom() >= b.top();
}

double crossProduct(const QPointF &a, const QPointF &b, const QPointF &c)
{
    return (b.x() - a.x()) * (c.y() - a.y())
           - (b.y() - a.y()) * (c.x() - a.x());
}

bool pointOnSegment(const QPointF &point, const QPointF &start, const QPointF &end)
{
    constexpr double epsilon = 1e-9;
    return qAbs(crossProduct(start, end, point)) <= epsilon
           && point.x() >= qMin(start.x(), end.x()) - epsilon
           && point.x() <= qMax(start.x(), end.x()) + epsilon
           && point.y() >= qMin(start.y(), end.y()) - epsilon
           && point.y() <= qMax(start.y(), end.y()) + epsilon;
}

bool segmentsIntersect(const QPointF &a, const QPointF &b,
                      const QPointF &c, const QPointF &d)
{
    const double abC = crossProduct(a, b, c);
    const double abD = crossProduct(a, b, d);
    const double cdA = crossProduct(c, d, a);
    const double cdB = crossProduct(c, d, b);
    if (((abC > 0.0 && abD < 0.0) || (abC < 0.0 && abD > 0.0))
        && ((cdA > 0.0 && cdB < 0.0) || (cdA < 0.0 && cdB > 0.0)))
        return true;
    return (abC == 0.0 && pointOnSegment(c, a, b))
           || (abD == 0.0 && pointOnSegment(d, a, b))
           || (cdA == 0.0 && pointOnSegment(a, c, d))
           || (cdB == 0.0 && pointOnSegment(b, c, d));
}

double segmentDistanceSquared(const QPointF &a, const QPointF &b,
                              const QPointF &c, const QPointF &d)
{
    if (segmentsIntersect(a, b, c, d))
        return 0.0;
    return qMin(qMin(pointSegmentDistanceSquared(a, c, d),
                     pointSegmentDistanceSquared(b, c, d)),
                qMin(pointSegmentDistanceSquared(c, a, b),
                     pointSegmentDistanceSquared(d, a, b)));
}

bool inkStrokeIntersectsPath(fz_context *ctx, pdf_annot *annot, int stroke,
                             const QVector<LineSegment> &pathSegments,
                             const QRectF &pathBounds, double radius)
{
    const double borderWidth = pdf_annot_border_width(ctx, annot);
    const double hitRadius = radius + qMax(0.0, borderWidth) / 2.0;
    const double hitRadiusSquared = hitRadius * hitRadius;
    const int vertexCount = pdf_annot_ink_list_stroke_count(ctx, annot, stroke);
    if (vertexCount <= 0)
        return false;

    const double left = pathBounds.left() - hitRadius;
    const double top = pathBounds.top() - hitRadius;
    const double right = pathBounds.right() + hitRadius;
    const double bottom = pathBounds.bottom() + hitRadius;
    const QRectF expandedPathBounds(QPointF(left, top), QPointF(right, bottom));

    fz_point firstVertex = pdf_annot_ink_list_stroke_vertex(ctx, annot, stroke, 0);
    double minX = firstVertex.x;
    double maxX = firstVertex.x;
    double minY = firstVertex.y;
    double maxY = firstVertex.y;
    for (int vertex = 1; vertex < vertexCount; ++vertex) {
        const fz_point point = pdf_annot_ink_list_stroke_vertex(ctx, annot, stroke, vertex);
        minX = qMin(minX, double(point.x));
        maxX = qMax(maxX, double(point.x));
        minY = qMin(minY, double(point.y));
        maxY = qMax(maxY, double(point.y));
    }
    if (!boundsIntersect(QRectF(QPointF(minX, minY), QPointF(maxX, maxY)),
                         expandedPathBounds))
        return false;

    if (vertexCount == 1) {
        const QPointF point(firstVertex.x, firstVertex.y);
        const QRectF inkBounds = segmentBounds(point, point);
        for (const LineSegment &pathSegment : pathSegments) {
            if (!boundsIntersect(inkBounds, pathSegment.bounds.adjusted(
                    -hitRadius, -hitRadius, hitRadius, hitRadius)))
                continue;
            if (pointSegmentDistanceSquared(point, pathSegment.start, pathSegment.end)
                <= hitRadiusSquared)
                return true;
        }
        return false;
    }

    for (int vertex = 1; vertex < vertexCount; ++vertex) {
        const fz_point previous = pdf_annot_ink_list_stroke_vertex(
            ctx, annot, stroke, vertex - 1);
        const fz_point current = pdf_annot_ink_list_stroke_vertex(ctx, annot, stroke, vertex);
        const QPointF inkStart(previous.x, previous.y);
        const QPointF inkEnd(current.x, current.y);
        const QRectF inkBounds = segmentBounds(inkStart, inkEnd);
        const QRectF expandedInkBounds = inkBounds.adjusted(
            -hitRadius, -hitRadius, hitRadius, hitRadius);
        for (const LineSegment &pathSegment : pathSegments) {
            if (!boundsIntersect(expandedInkBounds, pathSegment.bounds))
                continue;
            if (segmentDistanceSquared(pathSegment.start, pathSegment.end,
                                       inkStart, inkEnd) <= hitRadiusSquared)
                return true;
        }
    }
    return false;
}

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

bool PdfEditor::saveAs(const QString &path, bool removeUnused,
                       Encryption encryption, const QString &password)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }

    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray passUtf8 = password.toUtf8();
    const char *cPath = pathUtf8.constData();
    pdf_write_options opts = pdf_default_write_options;

    if (removeUnused)
        opts.do_garbage = 2;

    switch (encryption) {
    case Encryption::Keep:
        break;
    case Encryption::Remove:
        opts.do_encrypt = PDF_ENCRYPT_NONE;
        break;
    case Encryption::Aes256:
        if (passUtf8.isEmpty() || passUtf8.size() >= int(sizeof(opts.upwd_utf8))) {
            m_error = "The password must be between 1 and 127 characters.";
            return false;
        }
        opts.do_encrypt = PDF_ENCRYPT_AES_256;
        opts.permissions = -1;
        std::memset(opts.upwd_utf8, 0, sizeof(opts.upwd_utf8));
        std::memset(opts.opwd_utf8, 0, sizeof(opts.opwd_utf8));
        std::memcpy(opts.upwd_utf8, passUtf8.constData(), size_t(passUtf8.size()));
        std::memcpy(opts.opwd_utf8, passUtf8.constData(), size_t(passUtf8.size()));
        break;
    }

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

    constexpr int kMaxHitsPerPage = 65536;
    constexpr int kSearchCapacity = kMaxHitsPerPage + 1;
    std::vector<fz_quad> quads(kSearchCapacity);
    std::vector<int> marks(kSearchCapacity);
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
                                  marks.data(), quads.data(), kSearchCapacity);
            if (hits > kMaxHitsPerPage) {
                fz_throw(m_ctx, FZ_ERROR_GENERIC,
                         "Page %d has more than %d matches; highlighting was stopped.",
                         pageIndex + 1, kMaxHitsPerPage);
            }
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

bool PdfEditor::addInkStroke(int pageIndex, const QVector<QPointF> &points,
                             float red, float green, float blue, double width, double opacity)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (points.size() < 2) {
        m_error = "The stroke is too short.";
        return false;
    }

    std::vector<fz_point> stroke;
    stroke.reserve(points.size());
    for (const QPointF &p : points)
        stroke.push_back(fz_make_point(float(p.x()), float(p.y())));
    const int count = int(stroke.size());
    const float color[3] = { red, green, blue };

    pdf_page *page = nullptr;
    fz_var(page);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);

        pdf_annot *annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_INK);
        pdf_set_annot_color(m_ctx, annot, 3, color);
        pdf_set_annot_border(m_ctx, annot, float(width));
        pdf_set_annot_opacity(m_ctx, annot, float(opacity));
        pdf_add_annot_ink_list(m_ctx, annot, count, stroke.data());
        pdf_update_annot(m_ctx, annot);   // builds how the stroke looks
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

bool PdfEditor::eraseInk(int pageIndex, const QVector<QPointF> &path, double radius,
                         int *annotationsRemoved)
{
    if (annotationsRemoved)
        *annotationsRemoved = 0;
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (path.isEmpty() || radius <= 0.0) {
        m_error = "The eraser path or size is invalid.";
        return false;
    }

    QVector<LineSegment> pathSegments;
    pathSegments.reserve(qMax(1, path.size() - 1));
    QRectF pathBounds;
    if (path.size() == 1) {
        pathBounds = QRectF(path.first(), path.first());
        pathSegments.append({ path.first(), path.first(), pathBounds });
    } else {
        pathBounds = QRectF(path.first(), path.first());
        for (int point = 1; point < path.size(); ++point) {
            const QPointF start = path.at(point - 1);
            const QPointF end = path.at(point);
            const QRectF bounds = segmentBounds(start, end);
            pathBounds |= bounds;
            pathSegments.append({ start, end, bounds });
        }
    }

    pdf_page *page = nullptr;
    pdf_annot *annot = nullptr;
    pdf_annot *next = nullptr;
    int count = 0;
    int removed = 0;
    std::vector<int> keptStrokeCounts;
    std::vector<fz_point> keptVertices;
    fz_var(page);
    fz_var(annot);
    fz_var(next);
    fz_var(count);
    fz_var(removed);
    fz_var(keptStrokeCounts);
    fz_var(keptVertices);

    fz_try(m_ctx)
    {
        count = pdf_count_pages(m_ctx, m_doc);
        if (pageIndex < 0 || pageIndex >= count)
            fz_throw(m_ctx, FZ_ERROR_GENERIC, "That page does not exist.");

        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        for (annot = pdf_first_annot(m_ctx, page); annot; annot = next) {
            next = pdf_next_annot(m_ctx, annot);
            if (pdf_annot_type(m_ctx, annot) != PDF_ANNOT_INK)
                continue;

            keptStrokeCounts.clear();
            keptVertices.clear();
            int removedFromAnnotation = 0;
            const int strokeCount = pdf_annot_ink_list_count(m_ctx, annot);
            for (int stroke = 0; stroke < strokeCount; ++stroke) {
                const int vertexCount = pdf_annot_ink_list_stroke_count(
                    m_ctx, annot, stroke);
                if (inkStrokeIntersectsPath(
                        m_ctx, annot, stroke, pathSegments, pathBounds, radius)) {
                    ++removedFromAnnotation;
                    continue;
                }
                if (vertexCount <= 0)
                    continue;

                keptStrokeCounts.push_back(vertexCount);
                for (int vertex = 0; vertex < vertexCount; ++vertex)
                    keptVertices.push_back(pdf_annot_ink_list_stroke_vertex(
                        m_ctx, annot, stroke, vertex));
            }

            if (removedFromAnnotation == 0)
                continue;
            if (keptStrokeCounts.empty()) {
                pdf_delete_annot(m_ctx, page, annot);
            } else {
                pdf_set_annot_ink_list(m_ctx, annot, int(keptStrokeCounts.size()),
                                       keptStrokeCounts.data(), keptVertices.data());
                pdf_update_annot(m_ctx, annot);
            }
            removed += removedFromAnnotation;
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
        if (annotationsRemoved)
            *annotationsRemoved = removed;
        return false;
    }

    if (annotationsRemoved)
        *annotationsRemoved = removed;
    return true;
}

bool PdfEditor::addImageStamp(int pageIndex, const QRectF &box, const QByteArray &imageData)
{
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (imageData.isEmpty()) {
        m_error = "There is no image to add.";
        return false;
    }

    const unsigned char *data = reinterpret_cast<const unsigned char *>(imageData.constData());
    const size_t size = size_t(imageData.size());
    const fz_rect rect = fz_make_rect(float(box.left()), float(box.top()),
                                      float(box.right()), float(box.bottom()));

    pdf_page *page = nullptr;
    fz_buffer *buffer = nullptr;
    fz_image *image = nullptr;
    fz_var(page);
    fz_var(buffer);
    fz_var(image);

    fz_try(m_ctx)
    {
        buffer = fz_new_buffer_from_copied_data(m_ctx, data, size);
        image = fz_new_image_from_buffer(m_ctx, buffer);
        page = pdf_load_page(m_ctx, m_doc, pageIndex);

        pdf_annot *annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_STAMP);
        pdf_set_annot_rect(m_ctx, annot, rect);
        pdf_set_annot_stamp_image(m_ctx, annot, image);
        pdf_set_annot_rect(m_ctx, annot, rect);
        pdf_update_annot(m_ctx, annot);
    }
    fz_always(m_ctx)
    {
        if (image)
            fz_drop_image(m_ctx, image);
        if (buffer)
            fz_drop_buffer(m_ctx, buffer);
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