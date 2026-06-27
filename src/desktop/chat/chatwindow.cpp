// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktop/chat/chatwindow.h"

#include <QApplication>
#include <QDesktopWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace widgets {

ChatWindow::ChatWindow(
	QWidget *content, Qt::WindowFlags windowFlags, QWidget *parent,
	qreal opacity)
	: QWidget(parent, windowFlags)
	, m_opacity(opacity)
{
	setWindowTitle(tr("Chat"));
	setAttribute(Qt::WA_DeleteOnClose);
	setAttribute(Qt::WA_TranslucentBackground);
	if(windowFlags.testFlag(Qt::FramelessWindowHint)) {
		setWindowOpacity(m_opacity);
	}

	auto *layout = new QVBoxLayout;
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	setLayout(layout);

	m_titleBar = new ChatTitleBar(this);
	layout->addWidget(m_titleBar);
	layout->addWidget(content);
	content->installEventFilter(this);

	connect(m_titleBar, &ChatTitleBar::closeRequested, this, &QWidget::close);

	m_opacityStep = static_cast<int>((opacity - 0.3) / 0.1);
}

ChatWindow::~ChatWindow()
{
}

void ChatWindow::setOverlayOpacity(qreal opacity)
{
	m_opacity = opacity;
	setWindowOpacity(opacity);
}

qreal ChatWindow::overlayOpacity() const
{
	return m_opacity;
}

void ChatWindow::setOpacityStep(int step)
{
	m_opacityStep = step;
	m_opacity = 0.3 + step * 0.1;
	setWindowOpacity(m_opacity);
}

int ChatWindow::opacityStep() const
{
	return m_opacityStep;
}

void ChatWindow::wheelEvent(QWheelEvent *event)
{
	if(event->modifiers() & Qt::ControlModifier) {
		int newStep = m_opacityStep;
		if(event->angleDelta().y() > 0) {
			newStep = qMin(6, m_opacityStep + 1);
		} else {
			newStep = qMax(0, m_opacityStep - 1);
		}
		if(newStep != m_opacityStep) {
			setOpacityStep(newStep);
			emit opacityChanged(m_opacity);
		}
		event->accept();
	} else {
		QWidget::wheelEvent(event);
	}
}

ChatWindow::ResizeBorder ChatWindow::resizeBorderAt(const QPoint &pos) const
{
	ResizeBorder border = None;
	if(pos.x() <= RESIZE_MARGIN) {
		border = ResizeBorder(border | Left);
	}
	if(pos.x() >= width() - RESIZE_MARGIN) {
		border = ResizeBorder(border | Right);
	}
	if(pos.y() <= RESIZE_MARGIN) {
		border = ResizeBorder(border | Top);
	}
	if(pos.y() >= height() - RESIZE_MARGIN) {
		border = ResizeBorder(border | Bottom);
	}
	return border;
}

void ChatWindow::updateResizeBorder(const QPoint &pos)
{
	ResizeBorder border = resizeBorderAt(pos);
	if(border == None) {
		setCursor(Qt::ArrowCursor);
	} else if((border == Left) || (border == Right)) {
		setCursor(Qt::SizeHorCursor);
	} else if((border == Top) || (border == Bottom)) {
		setCursor(Qt::SizeVerCursor);
	} else if((border & Top) && (border & Left)) {
		setCursor(Qt::SizeFDiagCursor);
	} else if((border & Top) && (border & Right)) {
		setCursor(Qt::SizeBDiagCursor);
	} else if((border & Bottom) && (border & Left)) {
		setCursor(Qt::SizeBDiagCursor);
	} else if((border & Bottom) && (border & Right)) {
		setCursor(Qt::SizeFDiagCursor);
	} else {
		setCursor(Qt::ArrowCursor);
	}
}

void ChatWindow::updateCursor(const QPoint &pos)
{
	updateResizeBorder(pos);
}

void ChatWindow::mousePressEvent(QMouseEvent *event)
{
	QPoint pos = event->pos();
	m_resizeBorder = resizeBorderAt(pos);
	if(m_resizeBorder != None && event->button() == Qt::LeftButton) {
		m_resizing = true;
		m_dragStart = event->globalPos();
		m_initialGeometry = geometry();
		event->accept();
	} else if(event->button() == Qt::LeftButton) {
		m_dragStart = event->globalPos() - frameGeometry().topLeft();
		m_resizing = false;
		event->accept();
	} else {
		QWidget::mousePressEvent(event);
	}
}

void ChatWindow::mouseMoveEvent(QMouseEvent *event)
{
	if(m_resizing) {
		QPoint delta = event->globalPos() - m_dragStart;
		QRect geom = m_initialGeometry;
		if(m_resizeBorder & Left) {
			geom.setLeft(geom.left() + delta.x());
		}
		if(m_resizeBorder & Right) {
			geom.setRight(geom.right() + delta.x());
		}
		if(m_resizeBorder & Top) {
			geom.setTop(geom.top() + delta.y());
		}
		if(m_resizeBorder & Bottom) {
			geom.setBottom(geom.bottom() + delta.y());
		}
		QSize minSize = minimumSizeHint();
		if(geom.width() < minSize.width()) {
			if(m_resizeBorder & Left) {
				geom.setLeft(geom.right() - minSize.width() + 1);
			} else {
				geom.setWidth(minSize.width());
			}
		}
		if(geom.height() < minSize.height()) {
			if(m_resizeBorder & Top) {
				geom.setTop(geom.bottom() - minSize.height() + 1);
			} else {
				geom.setHeight(minSize.height());
			}
		}
		setGeometry(geom);
		event->accept();
	} else if(event->buttons() & Qt::LeftButton && !m_resizing) {
		move(event->globalPos() - m_dragStart);
		event->accept();
	}
	updateCursor(event->pos());
}

void ChatWindow::mouseReleaseEvent(QMouseEvent *event)
{
	m_resizing = false;
	m_resizeBorder = None;
	QWidget::mouseReleaseEvent(event);
}

void ChatWindow::enterEvent(QEvent *event)
{
	updateCursor(mapFromGlobal(QCursor::pos()));
	QWidget::enterEvent(event);
}

bool ChatWindow::eventFilter(QObject *watched, QEvent *event)
{
	if(event->type() == QEvent::MouseMove) {
		QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
		QPoint pos = mapFromGlobal(mouseEvent->globalPos());
		if(!m_resizing) {
			updateCursor(pos);
			// Set cursor on child widget to make resize handles visible
			if(QWidget *w = qobject_cast<QWidget *>(watched)) {
				w->setCursor(cursor());
			}
		} else {
			QPoint delta = mouseEvent->globalPos() - m_dragStart;
			QRect geom = m_initialGeometry;
			if(m_resizeBorder & Left) {
				geom.setLeft(geom.left() + delta.x());
			}
			if(m_resizeBorder & Right) {
				geom.setRight(geom.right() + delta.x());
			}
			if(m_resizeBorder & Top) {
				geom.setTop(geom.top() + delta.y());
			}
			if(m_resizeBorder & Bottom) {
				geom.setBottom(geom.bottom() + delta.y());
			}
			setGeometry(geom);
			mouseEvent->accept();
			return true;
		}
	} else if(event->type() == QEvent::MouseButtonPress) {
		QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
		QPoint pos = mapFromGlobal(mouseEvent->globalPos());
		m_resizeBorder = resizeBorderAt(pos);
		if(m_resizeBorder != None && mouseEvent->button() == Qt::LeftButton) {
			m_resizing = true;
			m_dragStart = mouseEvent->globalPos();
			m_initialGeometry = geometry();
			mouseEvent->accept();
			return true;
		} else if(mouseEvent->button() == Qt::LeftButton) {
			m_dragStart = mouseEvent->globalPos() - frameGeometry().topLeft();
			m_resizing = false;
			mouseEvent->accept();
			return true;
		}
	} else if(event->type() == QEvent::MouseButtonRelease) {
		m_resizing = false;
		m_resizeBorder = None;
	} else if(event->type() == QEvent::Wheel) {
		QWheelEvent *wheelEvent = static_cast<QWheelEvent *>(event);
		if(wheelEvent->modifiers() & Qt::ControlModifier) {
			int newStep = m_opacityStep;
			if(wheelEvent->angleDelta().y() > 0) {
				newStep = qMin(6, m_opacityStep + 1);
			} else {
				newStep = qMax(0, m_opacityStep - 1);
			}
			if(newStep != m_opacityStep) {
				setOpacityStep(newStep);
				emit opacityChanged(m_opacity);
				wheelEvent->accept();
				return true;
			}
		}
	}
	return QWidget::eventFilter(watched, event);
}

void ChatWindow::paintEvent(QPaintEvent *event)
{
	Q_UNUSED(event)
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	QRect borderRect = rect().adjusted(1, 1, -1, -1);
	painter.setPen(QPen(QColor(80, 80, 80), 1));
	painter.setBrush(QColor(45, 45, 45));
	painter.drawRoundedRect(borderRect, 6, 6);

	// Draw a resize handle indicator (diagonal lines) in bottom-right corner
	painter.setPen(QPen(QColor(150, 150, 150), 1));
	int handleSize = 12;
	int x = width() - RESIZE_MARGIN + 4;
	int y = height() - RESIZE_MARGIN + 4;
	for(int i = 0; i < 3; ++i) {
		painter.drawLine(x + i * 3, y + handleSize, x + handleSize, y + i * 3);
	}
}

void ChatWindow::closeEvent(QCloseEvent *event)
{
	emit closing();
	QWidget::closeEvent(event);
}

}
