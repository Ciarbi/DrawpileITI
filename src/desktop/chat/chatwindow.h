// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include "desktop/chat/chattitlebar.h"
#include <QPoint>
#include <QWidget>

namespace widgets {

class ChatWindow final : public QWidget
{
	Q_OBJECT
public:
	explicit ChatWindow(
		QWidget *content, Qt::WindowFlags windowFlags, QWidget *parent,
		qreal opacity = 0.92);
	~ChatWindow() override;

	void setOverlayOpacity(qreal opacity);
	qreal overlayOpacity() const;

	void setOpacityStep(int step);
	int opacityStep() const;

signals:
	void closing();
	void opacityChanged(qreal opacity);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void enterEvent(QEvent *event) override;

private:
	enum ResizeBorder {
		None = 0,
		Left = 1,
		Right = 2,
		Top = 4,
		Bottom = 8,
		TopLeft = Top | Left,
		TopRight = Top | Right,
		BottomLeft = Bottom | Left,
		BottomRight = Bottom | Right
	};

	ResizeBorder resizeBorderAt(const QPoint &pos) const;
	void updateResizeBorder(const QPoint &pos);
	void updateCursor(const QPoint &pos);

	static constexpr int RESIZE_MARGIN = 16;

	QPoint m_dragStart;
	ResizeBorder m_resizeBorder = None;
	bool m_resizing = false;
	QRect m_initialGeometry;
	qreal m_opacity;
	int m_opacityStep = 0;
	ChatTitleBar *m_titleBar = nullptr;
};

}

#endif // CHATWINDOW_H
