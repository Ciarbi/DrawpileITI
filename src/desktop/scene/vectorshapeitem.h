// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DESKTOP_SCENE_VECTORSHAPEITEM_H
#define DESKTOP_SCENE_VECTORSHAPEITEM_H
#include "desktop/scene/baseitem.h"
#include "libclient/tools/vectorshape.h"

namespace drawingboard {

class VectorShapeItem final : public BaseItem {
public:
	enum { Type = VectorShapeType };

	VectorShapeItem(const tools::VectorShape &shape, QGraphicsItem *parent);

	int type() const override { return Type; }

	QRectF boundingRect() const override;

	void setShape(const tools::VectorShape &shape);

	void paint(
		QPainter *painter, const QStyleOptionGraphicsItem *options,
		QWidget *widget) override;

private:
	tools::VectorShape m_shape;
};

}

#endif
