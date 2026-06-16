// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHATWINDOW_H
#define CHATWINDOW_H

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

signals:
	void closing();

protected:
	void closeEvent(QCloseEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

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

	static constexpr int RESIZE_MARGIN = 8;

	QPoint m_dragStart;
	ResizeBorder m_resizeBorder = None;
	bool m_resizing = false;
	QRect m_initialGeometry;
	qreal m_opacity;
};

}

#endif // CHATWINDOW_H
