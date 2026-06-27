// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktop/chat/chattitlebar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>

namespace widgets {

ChatTitleBar::ChatTitleBar(QWidget *parent)
	: QWidget(parent)
	, m_dragStart(0, 0)
{
	setFixedHeight(30);
	setStyleSheet(
		"ChatTitleBar {"
		"  background-color: #3d3d3d;"
		"  border-top-left-radius: 6px;"
		"  border-top-right-radius: 6px;"
		"  border-bottom: 1px solid #555;"
		"}"
		"QLabel {"
		"  color: #eff0f1;"
		"  font-weight: bold;"
		"  font-size: 11px;"
		"}"
		"QPushButton {"
		"  color: #aaa;"
		"  border: none;"
		"  font-size: 16px;"
		"  font-weight: bold;"
		"  padding: 0px;"
		"  width: 20px;"
		"  height: 20px;"
		"  background: transparent;"
		"}"
		"QPushButton:hover {"
		"  color: #da4453;"
		"}"
	);

	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(10, 4, 6, 4);
	layout->setSpacing(4);

	auto *title = new QLabel(tr("Chat"), this);
	layout->addWidget(title);

	layout->addStretch();

	m_closeButton = new QPushButton("×", this);
	m_closeButton->setFixedSize(20, 20);
	layout->addWidget(m_closeButton);

	connect(
		m_closeButton, &QPushButton::clicked, this,
		&ChatTitleBar::closeRequested);
}

void ChatTitleBar::mousePressEvent(QMouseEvent *event)
{
	if(event->button() == Qt::LeftButton && parentWidget()) {
		m_dragStart = event->globalPos() - parentWidget()->frameGeometry().topLeft();
		event->accept();
	} else {
		QWidget::mousePressEvent(event);
	}
}

void ChatTitleBar::mouseMoveEvent(QMouseEvent *event)
{
	if((event->buttons() & Qt::LeftButton) && parentWidget()) {
		parentWidget()->move(event->globalPos() - m_dragStart);
		event->accept();
	} else {
		QWidget::mouseMoveEvent(event);
	}
}

}
