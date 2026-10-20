#pragma once
#include <QByteArray>
#include <QSizeF>
#include <QString>

#include "PageView.h"

QByteArray renderTextToPng(const QString &text, const PageView::TextStyle &style,
                           const QSizeF &boxPoints);