// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>

/// Breaks a plain text tooltip into lines of at most `width` characters at word boundaries,
/// keeping its own line breaks. Rich text is returned as is (Qt wraps it already).
QString WrapToolTip(const QString& text, qsizetype width = 80);

/// Installed on the application: shows every widget's plain text tooltip wrapped by WrapToolTip,
/// so a long one is not one line across the screen.
class ToolTipWrapFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};
