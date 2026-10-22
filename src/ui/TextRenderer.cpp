#include "TextRenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QTextDocument>
#include <QTextOption>
#include <QtMath>

QByteArray renderTextToPng(const QString &text, const PageView::TextStyle &style,
                           const QSizeF &boxPoints)
{
    constexpr double kLayoutScale = 10.0;

    const double longest = qMax(boxPoints.width(), boxPoints.height());
    const double pixelsPerPoint = qMin(5.0, 4096.0 / qMax(1.0, longest));
    const QSize imageSize(qMax(1, qCeil(boxPoints.width() * pixelsPerPoint)),
                          qMax(1, qCeil(boxPoints.height() * pixelsPerPoint)));

    QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QFont font(style.family);
    font.setPixelSize(qMax(1, qRound(style.sizePoints * kLayoutScale)));
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::NoSubpixelAntialias);

    QTextDocument document;
    document.setDefaultFont(font);
    document.setDocumentMargin(0);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document.setDefaultTextOption(option);
    document.setTextWidth(boxPoints.width() * kLayoutScale);
    document.setPlainText(text);

    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    painter.scale(pixelsPerPoint / kLayoutScale, pixelsPerPoint / kLayoutScale);

    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, style.color);
    document.documentLayout()->draw(&painter, context);
    painter.end();

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}