// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QEvent>
#include <QHelpEvent>
#include <QStringList>
#include <QTextDocument>
#include <QToolTip>
#include <QWidget>

#include "yuzu/util/tooltip_wrap.h"

QString WrapToolTip(const QString& text, qsizetype width) {
    if (text.isEmpty() || Qt::mightBeRichText(text)) {
        return text;
    }
    QStringList lines;
    for (const QString& paragraph : text.split(QLatin1Char('\n'))) {
        QString line;
        for (const QString& word : paragraph.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (!line.isEmpty() && line.size() + 1 + word.size() > width) {
                lines.push_back(line);
                line.clear();
            }
            if (!line.isEmpty()) {
                line += QLatin1Char(' ');
            }
            line += word;
        }
        lines.push_back(line);
    }
    return lines.join(QLatin1Char('\n'));
}

bool ToolTipWrapFilter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::ToolTip || !watched->isWidgetType()) {
        return false;
    }
    auto* const widget = static_cast<QWidget*>(watched);
    const QString tip = widget->toolTip();
    const QString wrapped = WrapToolTip(tip);
    if (tip.isEmpty() || wrapped == tip) {
        return false;
    }
    const auto* const help = static_cast<QHelpEvent*>(event);
    QToolTip::showText(help->globalPos(), wrapped, widget, QRect{}, widget->toolTipDuration());
    return true;
}
