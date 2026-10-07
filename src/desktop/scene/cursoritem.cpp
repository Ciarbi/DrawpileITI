// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/scene/cursoritem.h"
#include <QPainter>
#include <QGuiApplication>
#include <QScreen>

namespace drawingboard {

CursorItem::CursorItem(QGraphicsItem *parent)
	: BaseItem(parent)
{
	setFlag(ItemIgnoresTransformations);
	setZValue(Z_CURSOR);
	updateVisibility();
}

QRectF CursorItem::boundingRect() const
{
	return m_bounds;
}

void CursorItem::setCursor(const QCursor &cursor)
{
	if(cursor.shape() == Qt::BitmapCursor) {
		refreshGeometry();
		m_cursor = cursor;
		
		// Get device pixel ratio for high DPI support
		qreal dpr = 1.0;
		if (QGuiApplication::primaryScreen()) {
			dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
		}
		
		// Scale hotspot by device pixel ratio (similar to Krita's approach)
		QPoint hotspot = cursor.hotSpot();
		QPointF scaledHotspot(hotspot.x() * dpr, hotspot.y() * dpr);
		QSizeF pixmapSize(cursor.pixmap().size());
		pixmapSize /= dpr;
		
		m_bounds = QRectF(-scaledHotspot, pixmapSize);
	} else {
		m_cursor = QCursor();
	}
	updateVisibility();
}

void CursorItem::setOnCanvas(bool onCanvas)
{
	m_onCanvas = onCanvas;
	updateVisibility();
}

void CursorItem::paint(
	QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
	painter->drawPixmap(m_bounds.topLeft(), m_cursor.pixmap());
}

void CursorItem::updateVisibility()
{
	setVisible(m_onCanvas && m_cursor.shape() == Qt::BitmapCursor);
	refresh();
}

}
