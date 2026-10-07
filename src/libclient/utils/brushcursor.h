// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LIBCLIENT_UTILS_BRUSHCURSOR_H
#define LIBCLIENT_UTILS_BRUSHCURSOR_H

#include <QCursor>
#include <QHash>
#include <QPair>
#include <QPoint>
#include <QPixmap>
#include <QIcon>
#include <QScreen>
#include <QWindow>
#include <QGuiApplication>
#include <QDebug>
#include <QGlobalStatic>

namespace utils {

class BrushCursorCache {
public:
    static BrushCursorCache *instance();

    QCursor load(const QString &cursorName, int width, int height, int hotspotX, int hotspotY);

    BrushCursorCache();

private:
    QCursor loadImpl(const QString &cursorName, int hotspotX, int hotspotY, int width, int height);

    QHash<QString, QPair<QPoint, QCursor>> m_cursorHash;
};

class BrushCursor {
public:
    // Predefined Qt cursors
    static QCursor arrowCursor();
    static QCursor upArrowCursor();
    static QCursor crossCursor();
    static QCursor roundCursor();
    static QCursor pixelBlackCursor();
    static QCursor pixelWhiteCursor();
    static QCursor waitCursor();
    static QCursor ibeamCursor();
    static QCursor sizeVerCursor();
    static QCursor sizeHorCursor();
    static QCursor sizeBDiagCursor();
    static QCursor sizeFDiagCursor();
    static QCursor sizeAllCursor();
    static QCursor blankCursor();
    static QCursor splitVCursor();
    static QCursor splitHCursor();
    static QCursor pointingHandCursor();

    // Brush-specific cursors
    static QCursor zoomSmoothCursor();
    static QCursor zoomDiscreteCursor();
    static QCursor rotateCanvasSmoothCursor();
    static QCursor rotateCanvasDiscreteCursor();
    static QCursor triangleLeftHandedCursor();
    static QCursor triangleRightHandedCursor();
    static QCursor eraserCursor();
    static QCursor moveCursor();
    static QCursor moveSelectionCursor();
    static QCursor handCursor();
    static QCursor openHandCursor();
    static QCursor closedHandCursor();
    static QCursor rotateCursor();
    static QCursor colorPickCursor();
    static QCursor layerPickCursor();

    // Load cursor from SVG/PNG resource with custom size and hotspot
    static QCursor load(const QString &cursorName, int hotspotX = -1, int hotspotY = -1);
    static QCursor loadWithSize(const QString &cursorName, int width, int height, int hotspotX = -1, int hotspotY = -1);

private:
    BrushCursor() = delete;
};

}

#endif // LIBCLIENT_UTILS_BRUSHCURSOR_H