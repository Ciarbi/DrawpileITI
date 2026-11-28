// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/scene/vectorshapeitem.h"
#include <QMarginsF>
#include <QPainter>

namespace drawingboard {

VectorShapeItem::VectorShapeItem(
	const tools::VectorShape &shape, QGraphicsItem *parent)
	: BaseItem(parent)
	, m_shape(shape)
{
}

QRectF VectorShapeItem::boundingRect() const
{
	const qreal margin = m_shape.width / 2.0 + 1.0;
	return m_shape.path.boundingRect().marginsAdded(
		QMarginsF(margin, margin, margin, margin));
}

void VectorShapeItem::setShape(const tools::VectorShape &shape)
{
	refreshGeometry();
	m_shape = shape;
	refresh();
}

void VectorShapeItem::paint(
	QPainter *painter, const QStyleOptionGraphicsItem *options, QWidget *widget)
{
	Q_UNUSED(options);
	Q_UNUSED(widget);

	QPen pen(m_shape.color, m_shape.width);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	painter->setPen(pen);
	painter->setBrush(m_shape.fill ? QBrush(m_shape.color) : QBrush(Qt::NoBrush));
	painter->drawPath(m_shape.path);
}

}
