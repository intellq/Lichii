// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__linux__) && !defined(__ANDROID__)
#include <QLayout>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace ConfigurationShared {

inline QScrollArea* CreateScrollArea(QWidget* content, QWidget* parent) {
    if (content->layout()) {
        content->layout()->setSizeConstraint(QLayout::SetMinimumSize);
    }
    auto* scroll = new QScrollArea(parent);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    return scroll;
}

inline void MakePageScrollable(QWidget* page) {
    auto* content = new QWidget;
    content->setLayout(page->layout());
    auto* scroll = CreateScrollArea(content, page);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

} // namespace ConfigurationShared
#endif
