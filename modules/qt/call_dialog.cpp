/**
 * @file qt/call_dialog.cpp Qt UI module -- unified call control dialog
 */
#include "call_dialog.h"
#include "call_history.h"
#include "qt_mod.h"

#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
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
#include <QWindow>
#ifdef HAVE_LAYERSHELL
#include <LayerShellQt/Window>
#endif


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
	 * an event filter. Inherits the system/Plasma Qt style.
	 * WindowStaysOnTopHint keeps it above other application windows. */
	setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
		       Qt::WindowStaysOnTopHint);
	setAttribute(Qt::WA_TranslucentBackground);
	/* The dialog itself is transparent — the inner #callPanel
	 * layer carries the themed background, border and radius. */
	setStyleSheet(
		"QFrame#callPanel { background-color: palette(window);"
		"         border: 2px solid #2a2e33;"
		"         border-radius: 8px; }");
	setAttribute(Qt::WA_ShowWithoutActivating, false);
	installEventFilter(this);
	buildUi();
	applyState();
	setupLayerShell();
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
	setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
		       Qt::WindowStaysOnTopHint);
	setAttribute(Qt::WA_TranslucentBackground);
	setStyleSheet(
		"QFrame#callPanel { background-color: palette(window);"
		"         border: 2px solid #2a2e33;"
		"         border-radius: 8px; }");
	installEventFilter(this);
	buildUi();
	uriEdit_->setText(peerUri);
	applyState();
	setupLayerShell();
}


void CallDialog::setupLayerShell()
{
#ifdef HAVE_LAYERSHELL
	/* Force native window creation to get a QWindow handle, then
	 * destroy the platform surface (xdg-shell) so LayerShellQt can
	 * install its event filter. When show() is called later, the
	 * filter intercepts surface creation and uses the layer-shell
	 * protocol instead — giving us compositor-side positioning and
	 * always-on-top (Overlay layer). */
	setAttribute(Qt::WA_NativeWindow);
	winId();

	QWindow *win = windowHandle();
	if (!win)
		return;

	/* Destroy the xdg-shell surface but keep the QWindow object. */
	win->destroy();

	auto *ls = LayerShellQt::Window::get(win);
	if (!ls)
		return;

	ls->setLayer(LayerShellQt::Window::LayerOverlay);
	ls->setKeyboardInteractivity(
		LayerShellQt::Window::KeyboardInteractivityOnDemand);
	ls->setScope("baresip-call-panel");
	ls->setCloseOnDismissed(true);
	layerShellApplied_ = true;
#endif
}


void CallDialog::positionNearTray()
{
	/* On Wayland, QSystemTrayIcon::geometry() is typically empty,
	 * so we infer the tray position from the screen's available
	 * geometry: the gap between the full screen geometry and the
	 * available geometry tells us where the panel bar is (top or
	 * bottom). We then position the popup just inside the available
	 * area, near the right edge (where tray icons usually live). */
	QRect iconGeo;
	if (trayIcon_)
		iconGeo = trayIcon_->geometry();

	QPoint pos;
	if (!iconGeo.isNull() && !iconGeo.isEmpty()) {
		pos = iconGeo.bottomLeft();
	} else {
		/* No tray geometry — infer from screen layout. */
		QScreen *screen = QGuiApplication::primaryScreen();
		if (!screen) {
			pos = QCursor::pos();
			screen = QGuiApplication::screenAt(pos);
		}
		if (!screen)
			screen = QGuiApplication::primaryScreen();
		if (!screen)
			return;

		QRect full = screen->geometry();
		QRect avail = screen->availableGeometry();

		/* Default to right edge. */
		int x = avail.right() - 20;

		/* If there's a gap at the top of the screen, the panel
		 * bar is at the top — drop the popup down from there. */
		if (avail.top() > full.top()) {
			pos = QPoint(x, avail.top());
		}
		/* If there's a gap at the bottom, panel is at the
		 * bottom — open the popup upward from above. */
		else if (avail.bottom() < full.bottom()) {
			pos = QPoint(x, avail.bottom());
		}
		else {
			pos = QCursor::pos();
		}
	}

	QSize sz = sizeHint();
	QScreen *scr = QGuiApplication::screenAt(pos);
	if (!scr)
		scr = QGuiApplication::primaryScreen();
	QRect avail = scr ? scr->availableGeometry()
			  : QRect(0, 0, 1920, 1080);

	int x = pos.x();
	int y = pos.y() - sz.height();
	if (x + sz.width() > avail.right())
		x = avail.right() - sz.width();
	if (x < avail.left())
		x = avail.left();
	if (y < avail.top())
		y = pos.y();

	move(x, y);
}


void CallDialog::fitWidthToHistory()
{
	/* Widen the dialog to fit the longest history entry without
	 * text wrap or ellipsis. Measure the text width with the
	 * list widget's actual font. */
	if (!historyList_ || historyList_->count() == 0)
		return;

	int maxWidth = 0;
	QFontMetrics fm(historyList_->font());
	for (int i = 0; i < historyList_->count(); ++i) {
		QListWidgetItem *it = historyList_->item(i);
		int iconWidth = it->icon().isNull() ? 0 : 24;
		maxWidth = std::max(maxWidth,
			fm.horizontalAdvance(it->text()) + iconWidth + 20);
	}

	/* Add space for the vertical scrollbar and frame margins. */
	maxWidth += style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 8;

	/* Don't shrink below the initial width (buttons + label). */
	if (maxWidth < 340)
		maxWidth = 340;

	resize(maxWidth, height());
}


void CallDialog::showPanel()
{
	/* Fit the dialog width to the history content before showing,
	 * so the full text of each entry is visible without ellipsis.
	 * Call after adjustSize() so adjustSize doesn't override it. */
	adjustSize();
	if (state_ == State::Dialing)
		fitWidthToHistory();

#ifdef HAVE_LAYERSHELL
	if (layerShellApplied_) {
		QWindow *win = windowHandle();
		if (win) {
			auto *ls = LayerShellQt::Window::get(win);
			if (ls) {
				/* Determine anchors and margins from the
				 * screen geometry. Detect which edge the
				 * panel bar is on by comparing full vs
				 * available geometry. */
				QScreen *screen = QGuiApplication::primaryScreen();
				QRect full = screen ? screen->geometry()
						    : QRect(0,0,1920,1080);
				QRect avail = screen ? screen->availableGeometry()
						     : QRect(0,0,1920,1080);

				int marginR, marginT = 0, marginB = 0;
				LayerShellQt::Window::Anchors anchors;

				/* Fixed top-right position with 60px top margin. */
				anchors = LayerShellQt::Window::Anchors(
					LayerShellQt::Window::AnchorTop |
					LayerShellQt::Window::AnchorRight);
				marginT = 60;

				marginR = 12;

				ls->setAnchors(anchors);
				ls->setMargins(QMargins(0, marginT, marginR, marginB));
				ls->setDesiredSize(size());
				show();
				return;
			}
		}
	}
#endif
	/* Fallback: plain frameless tool window, positioned manually. */
	positionNearTray();
	show();
	raise();
	activateWindow();
}


bool CallDialog::eventFilter(QObject *obj, QEvent *event)
{
	/* Close the panel when it loses focus (click-outside behavior
	 * to emulate Qt::Popup). Only in Dialing state -- incoming and
	 * in-call panels stay open until the call ends. */
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
	/* Outer layout leaves a gap so the dialog's own styled
	 * background/border shows as a frame around the panel. */
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(2, 2, 2, 2);

	/* Inner panel behind the displayed elements — no custom
	 * styling, it inherits the widget style (Qt6Curve). */
	auto *panel = new QFrame(this);
	panel->setObjectName("callPanel");
	panel->setAutoFillBackground(true);
	outer->addWidget(panel);

	auto *layout = new QVBoxLayout(panel);

	auto *label = new QLabel(panel);
	layout->addWidget(label);
	label->setText("Enter SIP URI or number:");

	uriEdit_ = new QLineEdit(panel);
	uriEdit_->setAlignment(Qt::AlignLeft);
	layout->addWidget(uriEdit_);

	/* In-call DTMF dialpad launcher (hidden unless InCall). */
	dialpadBtn_ = new QPushButton("Dialpad...", panel);
	layout->addWidget(dialpadBtn_);
	connect(dialpadBtn_, &QPushButton::clicked,
		this, &CallDialog::onDialpad);

	/* Call history list (shown only in Dialing state). */
	historyList_ = new QListWidget(panel);
	historyList_->setMaximumHeight(200);
	historyList_->setMinimumHeight(60);
	historyList_->setUniformItemSizes(true);
	/* Disable horizontal scrollbar — the dialog widens to fit the
	 * widest entry instead (see refreshHistory/adjustSize). */
	historyList_->setHorizontalScrollBarPolicy(
		Qt::ScrollBarAlwaysOff);
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

	/* Use a reasonable initial size; refreshHistory/adjustSize
	 * will widen the dialog to fit the longest history entry. */
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

		QString label;
		if (e.duration > 0) {
			/* Format duration as M:SS */
			int mins = e.duration / 60;
			int secs = e.duration % 60;
			QString dur = QString("%1:%2")
				.arg(mins)
				.arg(secs, 2, 10, QChar('0'));
			label = e.info.isEmpty()
				? QString("%1  (%3)  %2").arg(e.uri,
					e.ts.toString("MM-dd hh:mm"), dur)
				: QString("%1  (%3)  %2").arg(e.info,
					e.ts.toString("MM-dd hh:mm"), dur);
		} else {
			label = e.info.isEmpty()
				? QString("%1  %2").arg(e.uri,
					e.ts.toString("MM-dd hh:mm"))
				: QString("%1  %2").arg(e.info,
					e.ts.toString("MM-dd hh:mm"));
		}

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
