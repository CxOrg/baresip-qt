/**
 * @file qt/call_dialog.cpp Qt UI module -- unified call control dialog
 */
#include "call_dialog.h"
#include "call_history.h"
#include "qt_mod.h"

#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QIcon>
#include <QSystemTrayIcon>
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QCursor>


/* ---- green / red button helpers ---------------------------------- */

static QPushButton *makeButton(const QString &text,
				const QString &iconName,
				bool green)
{
	auto *btn = new QPushButton(text);
	btn->setMinimumSize(96, 44);

	QIcon ic = QIcon::fromTheme(iconName);
	if (!ic.isNull())
		btn->setIcon(ic);

	/* Tint via stylesheet so the colour is visible on any theme. */
	if (green)
		btn->setStyleSheet(
			"QPushButton { background-color: #2e7d32;"
			"              color: white;"
			"              font-weight: bold;"
			"              border-radius: 6px; }"
			"QPushButton:hover { background-color: #388e3c; }"
			"QPushButton:disabled { background-color: #666;"
			"                     color: #aaa; }");
	else
		btn->setStyleSheet(
			"QPushButton { background-color: #c62828;"
			"              color: white;"
			"              font-weight: bold;"
			"              border-radius: 6px; }"
			"QPushButton:hover { background-color: #d32f2f; }"
			"QPushButton:disabled { background-color: #666;"
			"                     color: #aaa; }");

	return btn;
}


/* ---- CallDialog --------------------------------------------------- */

CallDialog::CallDialog(QSystemTrayIcon *trayIcon, QWidget *parent)
	: QDialog(parent), state_(State::Dialing), trayIcon_(trayIcon)
{
	setWindowTitle("Dial");
	setAttribute(Qt::WA_DeleteOnClose, false);
	/* Frameless tool window that works on Wayland (unlike Qt::Popup,
	 * which needs a transient parent). Closes on click-outside via
	 * an event filter. Inherits the system/Plasma Qt style. */
	setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
	setAttribute(Qt::WA_ShowWithoutActivating, false);
	installEventFilter(this);
	buildUi();
	applyState();
}


CallDialog::CallDialog(State state, quintptr callPtr,
		       const QString &peerUri, const QString &peerName,
		       QSystemTrayIcon *trayIcon, QWidget *parent)
	: QDialog(parent), state_(state), callPtr_(callPtr),
	  peerName_(peerName), isOutgoing_(state == State::InCall &&
					  peerName.isEmpty()),
	  trayIcon_(trayIcon)
{
	setAttribute(Qt::WA_DeleteOnClose, false);
	setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
	installEventFilter(this);
	buildUi();
	uriEdit_->setText(peerUri);
	applyState();
}


void CallDialog::positionNearTray()
{
	/* Try to get the tray icon geometry. Under KDE Plasma/Wayland
	 * QSystemTrayIcon::geometry() often returns an empty rect, so
	 * fall back to the cursor position (the click that opened us). */
	QRect iconGeo;
	if (trayIcon_)
		iconGeo = trayIcon_->geometry();

	QPoint pos;
	if (!iconGeo.isNull() && !iconGeo.isEmpty()) {
		pos = iconGeo.bottomLeft();
	} else {
		/* Fall back to the current cursor position. */
		pos = QCursor::pos();
	}

	/* Move up so the panel opens upward from the icon (like a
	 * tray menu), and clamp to the nearest screen. */
	QSize sz = sizeHint();
	QScreen *screen = QGuiApplication::screenAt(pos);
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	QRect avail = screen ? screen->availableGeometry()
			     : QRect(0, 0, 1920, 1080);

	int x = pos.x();
	int y = pos.y() - sz.height();
	/* Clamp horizontally so the panel stays on-screen. */
	if (x + sz.width() > avail.right())
		x = avail.right() - sz.width();
	if (x < avail.left())
		x = avail.left();
	/* If it goes off the top, open downward instead. */
	if (y < avail.top())
		y = pos.y();

	move(x, y);
}


void CallDialog::showPanel()
{
	/* Position before showing so it appears in the right place. */
	adjustSize();
	positionNearTray();
	show();
	raise();
	activateWindow();
}


bool CallDialog::eventFilter(QObject *obj, QEvent *event)
{
	/* Close the panel when it loses focus (click-outside behavior
	 * to emulate Qt::Popup without Wayland's transient-parent
	 * requirement). Only do this in Dialing state -- incoming and
	 * in-call panels should stay open until the call ends. */
	if (obj == this && event->type() == QEvent::ActivationChange) {
		if (!isActiveWindow() && state_ == State::Dialing) {
			hide();
			return true;
		}
	}
	return QDialog::eventFilter(obj, event);
}


void CallDialog::buildUi()
{
	auto *layout = new QVBoxLayout(this);

	auto *label = new QLabel(this);
	layout->addWidget(label);
	label->setText("Enter SIP URI or number:");

	uriEdit_ = new QLineEdit(this);
	uriEdit_->setAlignment(Qt::AlignLeft);
	layout->addWidget(uriEdit_);

	/* In-call DTMF dialpad launcher (hidden unless InCall). */
	dialpadBtn_ = new QPushButton("Dialpad...", this);
	layout->addWidget(dialpadBtn_);
	connect(dialpadBtn_, &QPushButton::clicked,
		this, &CallDialog::onDialpad);

	/* Call history list (shown only in Dialing state). */
	historyList_ = new QListWidget(this);
	historyList_->setMaximumHeight(120);
	historyList_->setMinimumHeight(60);
	historyList_->setUniformItemSizes(true);
	/* Show ~10 entries before scrolling. */
	historyList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	layout->addWidget(historyList_);
	connect(historyList_, &QListWidget::itemClicked,
		this, &CallDialog::onHistoryClicked);
	connect(historyList_, &QListWidget::itemDoubleClicked,
		this, &CallDialog::onHistoryDoubleClicked);

	/* Button row: green on the left, red on the right. */
	auto *btnRow = new QHBoxLayout();
	layout->addLayout(btnRow);

	greenBtn_ = makeButton("Call", "call-start", true);
	redBtn_   = makeButton("Cancel", "call-stop", false);
	btnRow->addWidget(greenBtn_);
	btnRow->addWidget(redBtn_);

	connect(greenBtn_, &QPushButton::clicked,
		this, &CallDialog::onGreen);
	connect(redBtn_, &QPushButton::clicked,
		this, &CallDialog::onRed);

	resize(340, 320);
}


void CallDialog::applyState()
{
	switch (state_) {

	case State::Dialing:
		setWindowTitle("Dial");
		uriEdit_->setReadOnly(false);
		uriEdit_->setPlaceholderText("e.g. +441234567890");
		uriEdit_->clear();
		uriEdit_->setFocus();
		greenBtn_->setText("Call");
		greenBtn_->setEnabled(true);
		redBtn_->setText("Cancel");
		dialpadBtn_->hide();
		historyList_->show();
		refreshHistory();
		break;

	case State::Incoming:
		setWindowTitle(QString("Incoming call: %1").arg(peerName_));
		uriEdit_->setReadOnly(true);
		uriEdit_->setPlaceholderText(QString());
		greenBtn_->setText("Answer");
		greenBtn_->setEnabled(true);
		redBtn_->setText("Hangup");
		dialpadBtn_->hide();
		historyList_->hide();
		break;

	case State::InCall:
		setWindowTitle(QString("In call: %1").arg(
			uriEdit_->text().isEmpty() ? peerName_ : uriEdit_->text()));
		uriEdit_->setReadOnly(true);
		uriEdit_->setPlaceholderText(QString());
		/* Keep the green label from the call direction: "Call" for
		 * outgoing, "Answer" for incoming -- both disabled once
		 * connected. */
		greenBtn_->setText(isOutgoing_ ? "Call" : "Answer");
		greenBtn_->setEnabled(false);
		redBtn_->setText("Hangup");
		dialpadBtn_->show();
		historyList_->hide();
		break;
	}
}


void CallDialog::setStateInCall(const QString &peerUri)
{
	if (!peerUri.isEmpty())
		uriEdit_->setText(peerUri);
	state_ = State::InCall;
	applyState();
}


void CallDialog::setStateDialing()
{
	callPtr_ = 0;
	peerName_.clear();
	isOutgoing_ = false;
	state_ = State::Dialing;
	applyState();
}


void CallDialog::setStateIncoming(quintptr callPtr, const QString &peerUri,
				  const QString &peerName)
{
	callPtr_  = callPtr;
	peerName_ = peerName;
	uriEdit_->setText(peerUri);
	state_ = State::Incoming;
	applyState();
	show();
	raise();
	activateWindow();
}


void CallDialog::onGreen()
{
	switch (state_) {

	case State::Dialing: {
		QString uri = uriEdit_->text().trimmed();
		if (uri.isEmpty())
			return;
		emit callRequested(uri);
		/* The caller (TrayApp) will transition us to InCall once
		 * the outgoing call is created. */
		break;
	}

	case State::Incoming:
		emit answerRequested(callPtr_);
		/* TrayApp transitions us to InCall. */
		break;

	case State::InCall:
		/* Nothing -- already connected. */
		break;
	}
}


void CallDialog::onRed()
{
	switch (state_) {

	case State::Dialing:
		/* Just close -- no call was placed. */
		close();
		break;

	case State::Incoming:
		/* Reject = hangup the incoming call. */
		emit hangupRequested(callPtr_);
		break;

	case State::InCall:
		emit hangupRequested(callPtr_);
		break;
	}
}


void CallDialog::onDialpad()
{
	emit dialpadRequested(callPtr_,
		uriEdit_->text().isEmpty() ? peerName_ : uriEdit_->text());
}


void CallDialog::refreshHistory()
{
	if (!historyList_)
		return;

	historyList_->clear();

	/* Most recent first; show up to 10 entries. */
	auto entries = CallHistory::instance()->recent(10);
	/* Display newest at top. */
	for (int i = entries.size() - 1; i >= 0; --i) {
		const CallHistoryEntry &e = entries[i];

		QString iconName, fallback;
		switch (e.type) {
		case CALL_INCOMING:
			iconName = "call-incoming-symbolic"; fallback = "go-next";
			break;
		case CALL_OUTGOING:
			iconName = "call-outgoing-symbolic"; fallback = "go-previous";
			break;
		case CALL_MISSED:
			iconName = "call-missed-symbolic"; fallback = "call-stop";
			break;
		case CALL_REJECTED:
			iconName = "window-close"; fallback = "call-stop";
			break;
		default:
			iconName = "call-start"; fallback = QString();
			break;
		}

		QString label = e.info.isEmpty()
			? QString("%1  %2").arg(e.uri,
				e.ts.toString("MM-dd hh:mm"))
			: QString("%1  %2").arg(e.info,
				e.ts.toString("MM-dd hh:mm"));

		auto *item = new QListWidgetItem(label);
		QIcon ic = QIcon::fromTheme(iconName);
		if (ic.isNull() && !fallback.isEmpty())
			ic = QIcon::fromTheme(fallback);
		if (!ic.isNull())
			item->setIcon(ic);
		/* Stash the number for click-to-fill. The history CSV now
		 * stores just the phone number; the full SIP URI is
		 * reconstructed by qt_mod_connect on dialing. */
		item->setData(Qt::UserRole, e.uri);
		historyList_->addItem(item);
	}
}


void CallDialog::onHistoryClicked(QListWidgetItem *item)
{
	if (!item)
		return;
	QString uri = item->data(Qt::UserRole).toString();
	if (!uri.isEmpty())
		uriEdit_->setText(uri);
}


void CallDialog::onHistoryDoubleClicked(QListWidgetItem *item)
{
	if (!item)
		return;
	QString uri = item->data(Qt::UserRole).toString();
	if (uri.isEmpty())
		return;
	uriEdit_->setText(uri);
	/* Double-click = dial immediately. */
	onGreen();
}
