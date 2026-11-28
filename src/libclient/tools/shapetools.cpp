// SPDX-License-Identifier: GPL-3.0-or-later
#include "libclient/tools/shapetools.h"
#include "libclient/tools/toolcontroller.h"
#include "libclient/tools/utils.h"
#include "libclient/utils/cursors.h"

namespace tools {

static qreal brushWidth(const brushes::ActiveBrush &brush)
{
	qreal width;
	switch(brush.activeType()) {
	case brushes::ActiveBrush::CLASSIC:
		width = brush.classic().size.max;
		break;
	case brushes::ActiveBrush::MYPAINT:
		width = DP_mypaint_settings_max_size_for(
			&brush.myPaint().constSettings(), 1.0f);
		break;
	default:
		width = 1.0;
		break;
	}
	return qMax(qreal(1.0), width);
}

void ShapeTool::begin(const BeginParams &params)
{
	Q_ASSERT(!m_drawing);
	if(params.right) {
		return;
	}

	QPointF point = params.point;
	m_start = point;
	m_current = point;
	m_p1 = point;
	m_p2 = point;
	m_angle = params.angle;
	m_drawing = true;

	updatePreview();
}

void ShapeTool::motion(const MotionParams &params)
{
	if(m_drawing) {
		m_current = params.point;
		updateShape(params.constrain, params.center);
		updatePreview();
	}
}

void ShapeTool::modify(const ModifyParams &params)
{
	if(m_drawing) {
		updateShape(params.constrain, params.center);
		updatePreview();
	}
}

void ShapeTool::end(const EndParams &params)
{
	if(!m_drawing) {
		return;
	}

	updateShape(params.constrain, params.center);
	m_drawing = false;

	VectorShape shape = vectorShape(0);
	m_owner.addVectorShape(shape.path, shape.color, shape.width, shape.fill);
	clearPreview();
}

void ShapeTool::cancelMultipart()
{
	clearPreview();
	m_drawing = false;
}

void ShapeTool::setBrushSizeLimit(int limit)
{
	m_sizeLimit = limit;
}

void ShapeTool::setSelectionMaskingEnabled(bool selectionMaskingEnabled)
{
	setCapability(Capability::IgnoresSelections, !selectionMaskingEnabled);
}

QPointF ShapeTool::getConstrainPoint() const
{
	return constraints::square(m_start, m_current);
}

VectorShape ShapeTool::vectorShape(int id) const
{
	const brushes::ActiveBrush &brush = m_owner.activeBrush();
	VectorShape shape;
	shape.id = id;
	shape.path = path();
	shape.color = brush.qColor();
	shape.width = brushWidth(brush);
	if(m_sizeLimit > 0 && shape.width > m_sizeLimit) {
		shape.width = m_sizeLimit;
	}
	shape.fill = false;
	return shape;
}

void ShapeTool::clearPreview()
{
	VectorShape shape;
	shape.id = 0;
	emit m_owner.vectorShapePreviewRequested(shape);
}

void ShapeTool::updatePreview()
{
	emit m_owner.vectorShapePreviewRequested(vectorShape(0));
}

void ShapeTool::updateShape(bool constrain, bool center)
{
	m_p2 = constrain ? getConstrainPoint() : m_current;
	m_p1 = center ? m_start - (m_p2 - m_start) : m_start;
}

Line::Line(ToolController &owner, DP_MaskSync *ms)
	: ShapeTool(owner, LINE, utils::Cursors::line(), ms)
{
}

QPainterPath Line::path() const
{
	QPainterPath path;
	path.moveTo(m_p1);
	path.lineTo(m_p2);
	return path;
}

QPointF Line::getConstrainPoint() const
{
	return constraints::angle(m_start, m_current, angle());
}

Rectangle::Rectangle(ToolController &owner, DP_MaskSync *ms)
	: ShapeTool(owner, RECTANGLE, utils::Cursors::rectangle(), ms)
{
}

QPainterPath Rectangle::path() const
{
	QPainterPath path;
	path.moveTo(m_p1);
	path.lineTo(QPointF(m_p1.x(), m_p2.y()));
	path.lineTo(m_p2);
	path.lineTo(QPointF(m_p2.x(), m_p1.y()));
	path.closeSubpath();
	return path;
}

Ellipse::Ellipse(ToolController &owner, DP_MaskSync *ms)
	: ShapeTool(owner, ELLIPSE, utils::Cursors::ellipse(), ms)
{
}

QPainterPath Ellipse::path() const
{
	QRectF r = rect();
	QPainterPath path;
	if(r.isEmpty()) {
		path.moveTo(m_p1);
		path.lineTo(m_p2);
	} else {
		path.addEllipse(r);
	}
	return path;
}

Triangle::Triangle(ToolController &owner, DP_MaskSync *ms)
	: ShapeTool(owner, TRIANGLE, utils::Cursors::dot(), ms)
{
}

QPainterPath Triangle::path() const
{
	QRectF r = rect();
	QPainterPath path;
	path.moveTo(QPointF(r.center().x(), r.top()));
	path.lineTo(QPointF(r.right(), r.bottom()));
	path.lineTo(QPointF(r.left(), r.bottom()));
	path.closeSubpath();
	return path;
}

Diamond::Diamond(ToolController &owner, DP_MaskSync *ms)
	: ShapeTool(owner, DIAMOND, utils::Cursors::dot(), ms)
{
}

QPainterPath Diamond::path() const
{
	QRectF r = rect();
	QPainterPath path;
	path.moveTo(QPointF(r.center().x(), r.top()));
	path.lineTo(QPointF(r.right(), r.center().y()));
	path.lineTo(QPointF(r.center().x(), r.bottom()));
	path.lineTo(QPointF(r.left(), r.center().y()));
	path.closeSubpath();
	return path;
}

}
