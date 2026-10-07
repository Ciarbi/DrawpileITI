// SPDX-License-Identifier: GPL-3.0-or-later
#include "libclient/utils/brushcursor.h"

namespace utils {

Q_GLOBAL_STATIC(BrushCursorCache, s_brushCursorCacheInstance)

BrushCursorCache *BrushCursorCache::instance()
{
    return s_brushCursorCacheInstance;
}

BrushCursorCache::BrushCursorCache()
{
}

QCursor BrushCursorCache::loadImpl(const QString &cursorName, int hotspotX, int hotspotY, int width, int height)
{
#ifdef Q_OS_ANDROID
    (void)width;
    (void)height;
    QPixmap cursorImage = QPixmap(":cursors/" + cursorName);
#else
    QPixmap cursorImage = QIcon(":cursors/" + cursorName).pixmap(width > -1 ? width : 32, height > -1 ? height : 32);
#endif

    if (cursorImage.isNull()) {
        qWarning() << "Could not load cursor from qrc:" << cursorName;
        return Qt::ArrowCursor;
    }

    int hX = hotspotX;
    int hY = hotspotY;

#ifdef Q_OS_LINUX
    // On X11, we need to scale the hotspot by device pixel ratio
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == "xcb") {
        qreal dpr = cursorImage.devicePixelRatio();
        hX = (hotspotX >= 0 ? int(hotspotX * dpr) : int(cursorImage.width() / 2));
        hY = (hotspotY >= 0 ? int(hotspotY * dpr) : int(cursorImage.height() / 2));
    }
#endif

    return QCursor(cursorImage, hX, hY);
}

QCursor BrushCursorCache::load(const QString &cursorName, int width, int height, int hotspotX, int hotspotY)
{
    if (m_cursorHash.contains(cursorName)) {
        return m_cursorHash[cursorName].second;
    }

    QCursor newCursor = loadImpl(cursorName, hotspotX, hotspotY, width, height);
    m_cursorHash.insert(cursorName, QPair<QPoint, QCursor>(QPoint(hotspotX, hotspotY), newCursor));
    return newCursor;
}

// Predefined Qt cursors
QCursor BrushCursor::arrowCursor()
{
    return Qt::ArrowCursor;
}

QCursor BrushCursor::upArrowCursor()
{
    return Qt::UpArrowCursor;
}

QCursor BrushCursor::crossCursor()
{
    return loadWithSize("cross.png", 31, 31);
}

QCursor BrushCursor::roundCursor()
{
    return loadWithSize("round.png", 31, 31);
}

QCursor BrushCursor::pixelBlackCursor()
{
    return loadWithSize("pixel-black.png", 31, 31);
}

QCursor BrushCursor::pixelWhiteCursor()
{
    return loadWithSize("pixel-white.png", 31, 31);
}

QCursor BrushCursor::waitCursor()
{
    return Qt::WaitCursor;
}

QCursor BrushCursor::ibeamCursor()
{
    return Qt::IBeamCursor;
}

QCursor BrushCursor::sizeVerCursor()
{
    return Qt::SizeVerCursor;
}

QCursor BrushCursor::sizeHorCursor()
{
    return Qt::SizeHorCursor;
}

QCursor BrushCursor::sizeBDiagCursor()
{
    return Qt::SizeBDiagCursor;
}

QCursor BrushCursor::sizeFDiagCursor()
{
    return Qt::SizeFDiagCursor;
}

QCursor BrushCursor::sizeAllCursor()
{
    return Qt::SizeAllCursor;
}

QCursor BrushCursor::blankCursor()
{
    return Qt::BlankCursor;
}

QCursor BrushCursor::splitVCursor()
{
    return Qt::SplitVCursor;
}

QCursor BrushCursor::splitHCursor()
{
    return Qt::SplitHCursor;
}

QCursor BrushCursor::pointingHandCursor()
{
    return Qt::PointingHandCursor;
}

// Brush-specific cursors
QCursor BrushCursor::zoomSmoothCursor()
{
    return loadWithSize("zoom_smooth.svg", 33, 31);
}

QCursor BrushCursor::zoomDiscreteCursor()
{
    return loadWithSize("zoom_discrete.svg", 33, 31);
}

QCursor BrushCursor::rotateCanvasSmoothCursor()
{
    return loadWithSize("rotate_smooth.svg", 33, 31);
}

QCursor BrushCursor::rotateCanvasDiscreteCursor()
{
    return loadWithSize("rotate_discrete.svg", 33, 31);
}

QCursor BrushCursor::triangleLeftHandedCursor()
{
    return loadWithSize("triangle-left.png", 31, 31);
}

QCursor BrushCursor::triangleRightHandedCursor()
{
    return loadWithSize("triangle-right.png", 31, 31);
}

QCursor BrushCursor::eraserCursor()
{
    return loadWithSize("eraser.png", 32, 32, 2, 2);
}

QCursor BrushCursor::moveCursor()
{
    return loadWithSize("move.png", 24, 24);
}

QCursor BrushCursor::moveSelectionCursor()
{
    return loadWithSize("move-selection.png", 32, 32, 11, 11);
}

QCursor BrushCursor::handCursor()
{
    return Qt::PointingHandCursor;
}

QCursor BrushCursor::openHandCursor()
{
    return Qt::OpenHandCursor;
}

QCursor BrushCursor::closedHandCursor()
{
    return Qt::ClosedHandCursor;
}

QCursor BrushCursor::rotateCursor()
{
    return loadWithSize("rotate_cursor.svg", 22, 22);
}

QCursor BrushCursor::colorPickCursor()
{
    return loadWithSize("colorpicker.png", 33, 31, 8, 23);
}

QCursor BrushCursor::layerPickCursor()
{
    return loadWithSize("layerpicker.png", 33, 31, 8, 23);
}

// Generic load functions
QCursor BrushCursor::load(const QString &cursorName, int hotspotX, int hotspotY)
{
    return BrushCursorCache::instance()->load(cursorName, 32, 32, hotspotX, hotspotY);
}

QCursor BrushCursor::loadWithSize(const QString &cursorName, int width, int height, int hotspotX, int hotspotY)
{
    return BrushCursorCache::instance()->load(cursorName, width, height, hotspotX, hotspotY);
}

}