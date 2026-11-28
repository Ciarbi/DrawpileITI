// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LIBCLIENT_TOOLS_VECTORSHAPE_H
#define LIBCLIENT_TOOLS_VECTORSHAPE_H
#include <QColor>
#include <QMetaType>
#include <QPainterPath>

namespace tools {

/**
 * A locally owned vector shape. These are intentionally not sent over the
 * network: the Drawpile protocol has no vector layer type, so they live as a
 * client-side overlay layer for the current document.
 */
struct VectorShape {
	int id = 0;
	QPainterPath path;
	QColor color;
	qreal width = 1.0;
	bool fill = false;
};

}

Q_DECLARE_METATYPE(tools::VectorShape)

#endif
