// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHATTITLEBAR_H
#define CHATTITLEBAR_H

#include <QWidget>

class QPushButton;

namespace widgets {

class ChatTitleBar final : public QWidget {
	Q_OBJECT
public:
	explicit ChatTitleBar(QWidget *parent = nullptr);

signals:
	void closeRequested();

protected:
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;

private:
	QPoint m_dragStart;
	QPushButton *m_closeButton;
};

}

#endif // CHATTITLEBAR_H
