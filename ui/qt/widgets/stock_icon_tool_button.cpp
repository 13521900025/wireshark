/* stock_icon_tool_button.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <ui/qt/widgets/stock_icon_tool_button.h>

#include <ui/qt/utils/stock_icon.h>

#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QMouseEvent>

// We want nice icons that render correctly, and that are responsive
// when the user hovers and clicks them.
// Using setIcon renders correctly on normal and retina displays. It is
// not completely responsive, particularly on macOS.
// Calling setStyleSheet is responsive, but does not render correctly on
// retina displays: https://bugreports.qt.io/browse/QTBUG-36825
// Subclass QToolButton, which lets us catch events and set icons as needed.

StockIconToolButton::StockIconToolButton(QWidget * parent, QString stock_icon_name) :
    QToolButton(parent)
{
    setStockIcon(stock_icon_name);
}

void StockIconToolButton::setIconMode(QIcon::Mode mode)
{
    QIcon mode_icon;
    QList<QIcon::State> states = QList<QIcon::State>() << QIcon::Off << QIcon::On;
    foreach (QIcon::State state, states) {
        foreach (QSize size, base_icon_.availableSizes(mode, state)) {
            mode_icon.addPixmap(base_icon_.pixmap(size, mode, state), mode, state);
        }
    }
    setIcon(mode_icon);
}

void StockIconToolButton::setStockIcon(QString icon_name)
{
    if (!icon_name.isEmpty()) {
        icon_name_ = icon_name;
    }
    if (icon_name_.isEmpty()) {
        return;
    }
    base_icon_ = StockIcon(icon_name_);
    setIconMode();
}

bool StockIconToolButton::event(QEvent *event)
{
    switch (event->type()) {
        case QEvent::Enter:
        if (isEnabled()) {
            setIconMode(QIcon::Active);
        }
        break;
    case QEvent::MouseButtonPress:
        if (isEnabled()) {
            setIconMode(QIcon::Selected);
        }
        break;
    case QEvent::MouseButtonRelease:
        setIconMode();
        break;
    case QEvent::ApplicationPaletteChange:
        setStockIcon();
        break;
    default:
        break;
    }

    return QToolButton::event(event);
}
