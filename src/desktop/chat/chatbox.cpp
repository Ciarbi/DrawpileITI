// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/chat/chatbox.h"
#include "desktop/chat/chatwidget.h"
#include "desktop/chat/chatwindow.h"
#include "desktop/chat/useritemdelegate.h"
#include "desktop/utils/widgetutils.h"
#include "desktop/widgets/groupedtoolbutton.h"
#include "libclient/canvas/canvasmodel.h"
#include "libclient/canvas/userlist.h"
#include "libclient/document.h"
#include "libclient/net/client.h"
#include <QAction>
#include <QListView>
#include <QMetaObject>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QVBoxLayout>
#include <functional>

namespace widgets {

ChatBox::ChatBox(Document *doc, bool smallScreenMode, QWidget *parent)
	: QWidget(parent)
{
	QSplitter *chatsplitter = new QSplitter(Qt::Horizontal, this);
	chatsplitter->setChildrenCollapsible(false);
	m_chatWidget = new ChatWidget(smallScreenMode, this);
	chatsplitter->addWidget(m_chatWidget);

	QWidget *sidebar = new QWidget{this};
	QVBoxLayout *sidebarLayout = new QVBoxLayout;
	sidebarLayout->setContentsMargins(0, 0, 0, 0);
	sidebarLayout->setSpacing(0);
	sidebar->setLayout(sidebarLayout);

	QHBoxLayout *buttonsLayout = new QHBoxLayout;
	buttonsLayout->setContentsMargins(0, 0, 0, 0);
	buttonsLayout->setSpacing(0);
	sidebarLayout->addLayout(buttonsLayout);

	buttonsLayout->addStretch();

	m_inviteButton = new GroupedToolButton{GroupedToolButton::GroupLeft, this};
	m_inviteButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	m_inviteButton->setText(tr("Invite"));
	buttonsLayout->addWidget(m_inviteButton);

	m_sessionSettingsButton =
		new GroupedToolButton{GroupedToolButton::GroupCenter, this};
	m_sessionSettingsButton->setIcon(QIcon::fromTheme("configure"));
	m_sessionSettingsButton->setText(tr("Session"));
	buttonsLayout->addWidget(m_sessionSettingsButton);

	m_chatMenuButton =
		new GroupedToolButton{GroupedToolButton::GroupRight, this};
	m_chatMenuButton->setIcon(QIcon::fromTheme("edit-comment"));
	m_chatMenuButton->setText(tr("Chat"));
	m_chatMenuButton->setStatusTip(tr("Show chat options"));
	m_chatMenuButton->setToolTip(m_chatMenuButton->statusTip());
	m_chatMenuButton->setPopupMode(QToolButton::InstantPopup);
	m_chatMenuButton->setMenu(m_chatWidget->externalMenu());
	buttonsLayout->addWidget(m_chatMenuButton);

	m_pinButton = new GroupedToolButton{GroupedToolButton::NotGrouped, this};
	m_pinButton->setIcon(QIcon::fromTheme("pin"));
	m_pinButton->setToolTip(tr("Pin window on top"));
	m_pinButton->setCheckable(true);
	m_pinButton->setChecked(false);
	m_pinButton->setVisible(false);
	connect(m_pinButton, &QToolButton::clicked, this, &ChatBox::toggleDetachPin);
	buttonsLayout->addWidget(m_pinButton);

	buttonsLayout->addStretch();

	m_userList = new QListView(this);
	m_userList->setSelectionMode(QListView::NoSelection);
	m_userItemDelegate = new UserItemDelegate(this);
	m_userItemDelegate->setDocument(doc);
	m_userList->setItemDelegate(m_userItemDelegate);
	utils::bindKineticScrolling(m_userList);
	sidebarLayout->addWidget(m_userList);

	chatsplitter->addWidget(sidebar);

	chatsplitter->setStretchFactor(0, 5);
	chatsplitter->setStretchFactor(1, 1);

	auto *layout = new QVBoxLayout;
	layout->addWidget(chatsplitter);
	layout->setContentsMargins(0, 0, 0, 0);
	setLayout(layout);

	connect(m_chatWidget, &ChatWidget::message, this, &ChatBox::message);
	connect(
		m_chatWidget, &ChatWidget::detachRegularRequested, this,
		std::bind(&ChatBox::detachFromParent, this, DETACH_REGULAR));
	connect(
		m_chatWidget, &ChatWidget::detachOnTopRequested, this,
		std::bind(&ChatBox::detachFromParent, this, DETACH_ON_TOP));
	connect(
		m_chatWidget, &ChatWidget::detachAlwaysOnTopRequested, this,
		std::bind(&ChatBox::detachFromParent, this, DETACH_ALWAYS_ON_TOP));
	connect(
		m_chatWidget, &ChatWidget::detachOverlayRequested, this,
		std::bind(&ChatBox::detachFromParent, this, DETACH_OVERLAY));
	connect(m_chatWidget, &ChatWidget::expandRequested, this, [this]() {
		if(isCollapsed()) {
			emit expandPlease();
		}
	});
	connect(
		m_chatWidget, &ChatWidget::muteChanged, this, &ChatBox::muteChanged);
	connect(
		m_chatWidget, &ChatWidget::requestChatPositionTop, this,
		&ChatBox::setChatPositionTop);
	connect(
		m_chatWidget, &ChatWidget::requestChatPositionBottom, this,
		&ChatBox::setChatPositionBottom);

	connect(doc, &Document::canvasChanged, this, &ChatBox::onCanvasChanged);
	connect(doc, &Document::serverLoggedIn, this, &ChatBox::onServerLogin);

	connect(
		doc, &Document::sessionPreserveChatChanged, m_chatWidget,
		&ChatWidget::setPreserveMode);
	connect(
		doc->client(), &net::Client::serverMessage, m_chatWidget,
		&ChatWidget::receiveSystemMessage);

	connect(
		m_userItemDelegate, &widgets::UserItemDelegate::opCommand,
		doc->client(), &net::Client::sendMessage);
	connect(
		m_userItemDelegate, &widgets::UserItemDelegate::requestPrivateChat,
		m_chatWidget, &ChatWidget::openPrivateChat);
	connect(
		m_userItemDelegate, &widgets::UserItemDelegate::requestUserInfo, this,
		&ChatBox::requestUserInfo);
	connect(
		m_userItemDelegate, &widgets::UserItemDelegate::requestCurrentBrush,
		this, &ChatBox::requestCurrentBrush);
}

void ChatBox::setActions(QAction *inviteAction, QAction *sessionSettingsAction)
{
	const QPair<QAction *, QAbstractButton *> pairs[] = {
		{inviteAction, m_inviteButton},
		{sessionSettingsAction, m_sessionSettingsButton},
	};
	for(const QPair<QAction *, QAbstractButton *> &pair : pairs) {
		QAction *action = pair.first;
		QAbstractButton *button = pair.second;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		connect(action, &QAction::enabledChanged, button, &QWidget::setEnabled);
#else
		connect(action, &QAction::changed, this, [=] {
			button->setEnabled(action->isEnabled());
		});
#endif
		connect(button, &QAbstractButton::clicked, action, &QAction::trigger);
		button->setIcon(action->icon());
		button->setStatusTip(action->statusTip());
		button->setToolTip(action->statusTip());
		button->setEnabled(action->isEnabled());
	}
}

void ChatBox::setSmallScreenMode(bool smallScreenMode)
{
	m_chatWidget->setSmallScreenMode(smallScreenMode);
	if(smallScreenMode && m_state == State::Detached) {
		ChatWindow *window = qobject_cast<ChatWindow *>(parent());
		if(window) {
			window->close();
		}
	}
}

void ChatBox::onCanvasChanged(canvas::CanvasModel *canvas)
{
	m_userList->setModel(canvas->userlist()->onlineUsers());
	m_chatWidget->setModel(canvas);

	connect(
		canvas, &canvas::CanvasModel::chatMessageReceived, m_chatWidget,
		&ChatWidget::receiveMessage);
	connect(
		canvas, &canvas::CanvasModel::pinnedMessageChanged, m_chatWidget,
		&ChatWidget::setPinnedMessage);
	connect(
		canvas, &canvas::CanvasModel::userJoined, m_chatWidget,
		&ChatWidget::userJoined);
	connect(
		canvas, &canvas::CanvasModel::userLeft, m_chatWidget,
		&ChatWidget::userParted);
}

void ChatBox::onServerLogin()
{
	m_chatWidget->loggedIn(static_cast<Document *>(sender())->client()->myId());
}

void ChatBox::focusInput()
{
	m_chatWidget->focusInput();
}

void ChatBox::receiveSystemMessage(const QString &message, int type)
{
	m_chatWidget->receiveSystemMessage(message, type);
}

void ChatBox::setCurrentLayer(int layerId)
{
	m_chatWidget->setCurrentLayer(layerId);
}

void ChatBox::setChatPositionTop()
{
	emit requestChatPositionTop();
}

void ChatBox::setChatPositionBottom()
{
	emit requestChatPositionBottom();
}

void ChatBox::detachFromParent(int mode)
{
	QWidget *oldParent = parentWidget();
	if(!oldParent || qobject_cast<ChatWindow *>(oldParent)) {
		return;
	}

	m_state = State::Detached;
	m_overlayMode = (mode == DETACH_OVERLAY);

	QSize siz = size();

	m_chatWidget->setAttached(false);

	Qt::WindowFlags windowFlags = Qt::Window | Qt::FramelessWindowHint;
	QWidget *windowParent = nullptr;
	QString pinTip = tr("Pin window on top");

	if(mode == DETACH_ON_TOP) {
		windowParent = oldParent;
	} else if(mode == DETACH_ALWAYS_ON_TOP) {
		windowFlags.setFlag(Qt::WindowStaysOnTopHint);
		m_pinButton->setChecked(true);
		pinTip = tr("Unpin window");
	} else if(mode == DETACH_OVERLAY) {
		windowFlags.setFlag(Qt::WindowStaysOnTopHint);
		pinTip = tr("Disable overlay mode");
	}

	ChatWindow *window = new ChatWindow(this, windowFlags, windowParent, 
		m_overlayMode ? 0.5 : 0.92);
	connect(window, &ChatWindow::closing, this, &ChatBox::reattachToParent);
	connect(oldParent, &QObject::destroyed, window, &QObject::deleteLater);

	window->show();
	window->resize(siz);
	m_pinButton->setVisible(true);
	m_pinButton->setChecked(mode == DETACH_OVERLAY || mode == DETACH_ALWAYS_ON_TOP);
	m_pinButton->setToolTip(pinTip);
}

void ChatBox::reattachToParent()
{
	emit reattachNowPlease();
	m_state = State::Expanded;
	m_chatWidget->setAttached(true);
	m_overlayMode = false;
	m_pinButton->setChecked(false);
	m_pinButton->setVisible(false);
}

void ChatBox::toggleDetachPin(bool checked)
{
	QWidget *win = parentWidget();
	while(win && !qobject_cast<ChatWindow *>(win)) {
		win = win->parentWidget();
	}
	ChatWindow *cw = qobject_cast<ChatWindow *>(win);
	if(!cw) {
		return;
	}
	if(checked) {
		cw->setWindowFlag(Qt::WindowStaysOnTopHint);
		m_pinButton->setToolTip(m_overlayMode ? tr("Disable overlay mode") : tr("Unpin window"));
	} else {
		cw->setWindowFlag(Qt::WindowStaysOnTopHint, false);
		m_pinButton->setToolTip(m_overlayMode ? tr("Disable overlay mode") : tr("Pin window on top"));
	}
	cw->show();
	cw->raise();
	cw->activateWindow();
}

void ChatBox::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if(event->size().height() == 0) {
		if(m_state == State::Expanded) {
			emit expandedChanged(false);
			m_state = State::Collapsed;
		}

	} else if(m_state == State::Collapsed) {
		m_state = State::Expanded;
		emit expandedChanged(true);
	}
}

}
