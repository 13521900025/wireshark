/* geometry_state_dialog.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "geometry_state_dialog.h"
#include <ui/qt/utils/qt_ui_utils.h>
#include "ui/recent.h"
#include "ui/ws_ui_util.h"

GeometryStateDialog::~GeometryStateDialog()
{
    saveWindowGeometry();
}

void GeometryStateDialog::loadGeometry(int width, int height, const QString &dialog_name)
{
    window_geometry_t geom;

    dialog_name_ = dialog_name.isEmpty() ? objectName() : dialog_name;
    if (!dialog_name_.isEmpty() && window_geom_load(dialog_name_.toUtf8().constData(), &geom)) {
        if (geom.qt_geom == nullptr || !restoreGeometry(QByteArray::fromHex(geom.qt_geom))) {
            // restoreGeometry didn't work, fallback to older (but other
            // toolkit compatible?) less-accurate method. (restoreGeometry
            // is supposed to take care of things like making sure the window
            // is on screen, setting the non-maximized size if maximized, etc.)
            QRect recent_geom(geom.x, geom.y, geom.width, geom.height);

            // Check if the dialog is visible on any screen
            if (rect_on_screen(recent_geom)) {
                move(recent_geom.topLeft());
                resize(recent_geom.size());
            } else {
                // Not visible, move within a reasonable area and try size only
                recent_geom.moveTopLeft(QPoint(50, 50));
                if (rect_on_screen(recent_geom)) {
                    resize(recent_geom.size());
                } else if (width > 0 && height > 0) {
                    // We're not visible on any screens, use defaults
                    resize(width, height);
                }
            }
            if (geom.maximized) {
                showFullScreen();
            }
        }
    } else if (width > 0 && height > 0) {
        // No saved geometry found, use defaults
        resize(width, height);
    }
}

void GeometryStateDialog::saveWindowGeometry()
{
    if (dialog_name_.isEmpty())
        return;

    window_geometry_t geom;

    geom.key = NULL;
    geom.set_pos = TRUE;
    geom.x = pos().x();
    geom.y = pos().y();
    geom.set_size  = TRUE;
    geom.width = size().width();
    geom.height = size().height();
    geom.set_maximized = TRUE;
    // XXX: maximized and fullScreen are different window states; we've been
    // using the maximized key for fullScreen ever since this was added.
    geom.maximized = isFullScreen();
    geom.qt_geom = g_strdup(saveGeometry().toHex().constData());

    window_geom_save(dialog_name_.toUtf8().constData(), &geom);
}
