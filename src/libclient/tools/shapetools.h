// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LIBCLIENT_TOOLS_SHAPETOOLS_H
#define LIBCLIENT_TOOLS_SHAPETOOLS_H
#include "libclient/tools/tool.h"
#include "libclient/tools/vectorshape.h"
#include <QRectF>

struct DP_MaskSync;

namespace tools {

/**
 * \brief Base class for tools that create vector shapes
 *
 * Shapes are kept as a local overlay layer for the current document. They are
 * not painted into the selected raster layer and are not sent over the
 * network, since the protocol has no vector layer type.
 */
class ShapeTool : public Tool {
public:
	ShapeTool(ToolController &owner, Type type, QCursor cursor, DP_MaskSync *ms)
		: Tool(
			  owner, type, cursor,
			  Capability::AllowColorPick | Capability::Fractional |
				  Capability::AllowToolAdjust1 | Capability::AllowToolAdjust2 |
				  Capability::AllowToolAdjust3)
	{
		Q_UNUSED(ms);
	}

	void begin(const BeginParams &params) final override;
	void motion(const MotionParams &params) final override;
	void modify(const ModifyParams &params) final override;
	void end(const EndParams &params) final override;

	void cancelMultipart() final override;
	bool usesBrushColor() const final override { return true; }
	bool updatesBrushHistory() const final override { return true; }
	void setBrushSizeLimit(int limit) override;
	void setSelectionMaskingEnabled(bool selectionMaskingEnabled) override;

protected:
	//! Vector geometry of the shape.
	virtual QPainterPath path() const = 0;
	virtual QPointF getConstrainPoint() const;
	QRectF rect() const { return QRectF(m_p1, m_p2).normalized(); }
	qreal angle() const { return m_angle; }

	QPointF m_start, m_current, m_p1, m_p2;
	bool m_drawing = false;

private:
	VectorShape vectorShape(int id) const;
	void clearPreview();
	void updatePreview();
	void updateShape(bool constrain, bool center);

	qreal m_angle = 0.0;
	int m_sizeLimit = 0;
};

/**
 * \brief Line tool
 *
 * The line tool creates a straight vector line.
 */
class Line final : public ShapeTool {
public:
	Line(ToolController &owner, DP_MaskSync *ms);

protected:
	QPainterPath path() const override;
	QPointF getConstrainPoint() const override;
};

/**
 * \brief Rectangle drawing tool
 *
 * This tool creates vector squares and rectangles
 */
class Rectangle final : public ShapeTool {
public:
	Rectangle(ToolController &owner, DP_MaskSync *ms);

protected:
	QPainterPath path() const override;
};

/**
 * \brief Ellipse drawing tool
 *
 * This tool creates vector circles and ellipses
 */
class Ellipse final : public ShapeTool {
public:
	Ellipse(ToolController &owner, DP_MaskSync *ms);

protected:
	QPainterPath path() const override;
};

/**
 * \brief Triangle drawing tool
 */
class Triangle final : public ShapeTool {
public:
	Triangle(ToolController &owner, DP_MaskSync *ms);

protected:
	QPainterPath path() const override;
};

/**
 * \brief Diamond drawing tool
 *
 * This tool creates vector diamonds (rhombuses)
 */
class Diamond final : public ShapeTool {
public:
	Diamond(ToolController &owner, DP_MaskSync *ms);

protected:
	QPainterPath path() const override;
};

}

#endif
