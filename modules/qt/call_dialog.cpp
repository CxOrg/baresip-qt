/**
 * @file qt/call_dialog.cpp Qt UI module -- unified call control dialog
 */
#include "call_dialog.h"
#include "call_history.h"
#include "qt_mod.h"

#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPalette>
#include <QIcon>
#include <QSystemTrayIcon>
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QCursor>
#include <QWindow>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QResizeEvent>
#include <QHideEvent>
#include <algorithm>
#ifdef HAVE_LAYERSHELL
#include <LayerShellQt/Window>
#endif


/* ---- contacts file helpers (moved from contacts_dialog.cpp) ------- */

static QString contactsPath()
{
	return QDir::homePath() + "/.baresip/contacts";
}

/** Parse a contacts-file line into name/uri/params. Handles
 *  `"Name" <sip:user@host>;params`, bare `<sip:...>` and plain
 *  `sip:...` lines. Returns false for comments/blank lines. */
static bool parseContactLine(const QString &line,
			     CallDialog::ContactEntry &e)
{
	QString t = line.trimmed();
	if (t.isEmpty() || t.startsWith('#'))
		return false;

	int lt = t.indexOf('<');
	int gt = t.indexOf('>', lt);
	if (lt >= 0 && gt > lt) {
		e.name = t.left(lt).trimmed();
		if (e.name.startsWith('"') && e.name.endsWith('"')
		    && e.name.length() > 1)
			e.name = e.name.mid(1, e.name.length() - 2);
		e.uri    = t.mid(lt + 1, gt - lt - 1);
		e.params = t.mid(gt + 1).trimmed();
	}
	else {
		e.name.clear();
		e.uri = t;
		e.params.clear();
	}
	return true;
}

/** Render a ContactEntry back to a contacts-file line. */
static QString contactLine(const CallDialog::ContactEntry &e)
{
	QString line;
	if (!e.name.isEmpty())
		line = QString("\"%1\" ").arg(e.name);
	line += "<" + e.uri + ">";
	if (!e.params.isEmpty())
		line += e.params;
	return line;
}

/** Host part of a sip URI ("sip:u@host:port;p" -> "host:port"). */
static QString uriHost(const QString &uri)
{
	QString s = uri;
	if (s.startsWith("sip:", Qt::CaseInsensitive))
		s = s.mid(4);
	else if (s.startsWith("sips:", Qt::CaseInsensitive))
		s = s.mid(5);
	int at = s.indexOf('@');
	if (at < 0)
		return QString();
	s = s.mid(at + 1);
	int semi = s.indexOf(';');
	if (semi >= 0)
		s = s.left(semi);
	return s;
}

/** Build a full sip: URI for a bare number — prefer keeping the
 *  original host when editing, else complete via the current
 *  account's domain, else just "sip:<number>". */
static QString completeUri(const QString &number, const QString &origHost)
{
	QString host = origHost;
	if (!host.isEmpty())
		return QString("sip:%1@%2").arg(number, host);

	struct ua *ua = qt_current_ua();
	if (ua) {
		char *s = NULL;
		struct pl pl;
		QByteArray utf8 = number.toUtf8();
		pl.p = utf8.constData();
		pl.l = (size_t)utf8.size();
		if (0 == account_uri_complete_strdup(ua_account(ua),
						     &s, &pl)) {
			QString uri = QString::fromUtf8(s);
			mem_deref(s);
			return uri;
		}
	}
	return "sip:" + number;
}

/** A usable contact number needs at least 4 digits. */
static bool validNumber(const QString &s)
{
	int digits = 0;
	for (const QChar c : s) {
		if (c.isDigit())
			++digits;
	}
	return digits >= 4;
}

/** True when the edit field holds a SIP URI rather than a bare
 *  number — "sip:alice@host", "sips:..." or "alice@host". */
static bool isUriInput(const QString &s)
{
	return s.contains('@') ||
		s.startsWith("sip:", Qt::CaseInsensitive) ||
		s.startsWith("sips:", Qt::CaseInsensitive);
}


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
	 * layer carries the themed background, mid-grey border and
	 * radius. The #callPanel ID selector scopes the rule so
	 * the border does not inherit to child widgets. */
	setStyleSheet(
		"QFrame#callPanel { background-color: palette(window);"
		"         border: 2px solid #808080;"
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
		"         border: 2px solid #808080;"
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
	if (maxWidth < 470)
		maxWidth = 470;

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
				/* Top + nearest horizontal edge, margins
				 * from the tray-panel height. */
				qtPanelApplyAnchors(win, anchorPos_);
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
	panel_ = panel;

	auto *layout = new QVBoxLayout(panel);

	/* Top row: label + toggle switching the list between the
	 * call history and the contacts list. */
	auto *topRow = new QHBoxLayout();
	auto *label = new QLabel(panel);
	label->setText("Enter SIP URI or number:");
	topRow->addWidget(label);
	topRow->addStretch();
	listToggleBtn_ = new QPushButton("History/Contact", panel);
	topRow->addWidget(listToggleBtn_);
	layout->addLayout(topRow);
	connect(listToggleBtn_, &QPushButton::clicked,
		this, &CallDialog::onToggleList);

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

	/* Same width as the settings/contacts panels; fitWidthToHistory
	 * still expands it if a list entry needs more room. The
	 * minimum keeps adjustSize() from collapsing the dialog in
	 * InCall/Incoming state where the history list is hidden. */
	setMinimumWidth(470);
	resize(470, 320);
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
		refreshList();
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


void CallDialog::setDialNumber(const QString &number)
{
	uriEdit_->setText(number);
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

		/* Display/dial target: the number when the peer has a
		 * dialable number, else the full SIP URI. */
		QString target = e.target();

		QString label;
		if (e.duration > 0) {
			/* Format duration as M:SS */
			int mins = e.duration / 60;
			int secs = e.duration % 60;
			QString dur = QString("%1:%2")
				.arg(mins)
				.arg(secs, 2, 10, QChar('0'));
			label = e.info.isEmpty()
				? QString("%1  (%3)  %2").arg(target,
					e.ts.toString("MM-dd hh:mm"), dur)
				: QString("%1  (%3)  %2").arg(e.info,
					e.ts.toString("MM-dd hh:mm"), dur);
		} else {
			label = e.info.isEmpty()
				? QString("%1  %2").arg(target,
					e.ts.toString("MM-dd hh:mm"))
				: QString("%1  %2").arg(e.info,
					e.ts.toString("MM-dd hh:mm"));
		}

		auto *item = new QListWidgetItem();
		/* Stash the target for click-to-fill — a bare number is
		 * completed with the account domain on dialing, a full
		 * sip: URI is dialed directly. The remaining roles feed
		 * the row's add-contact/delete buttons. */
		item->setData(Qt::UserRole,     target);
		item->setData(Qt::UserRole + 1, e.ts);
		item->setData(Qt::UserRole + 2, e.number);
		item->setData(Qt::UserRole + 3, e.uri);
		item->setData(Qt::UserRole + 4, e.info);
		historyList_->addItem(item);

		QIcon ic = QIcon::fromTheme(iconName);
		if (ic.isNull() && !fallback.isEmpty())
			ic = QIcon::fromTheme(fallback);
		QWidget *row = makeRow(label, ic, item);
		item->setSizeHint(row->sizeHint());
		historyList_->setItemWidget(item, row);
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


void CallDialog::onToggleList()
{
	showingContacts_ = !showingContacts_;
	refreshList();
}


void CallDialog::refreshList()
{
	if (showingContacts_)
		refreshContacts();
	else
		refreshHistory();
}


void CallDialog::refreshContacts()
{
	if (!historyList_)
		return;

	historyList_->clear();
	loadContactsFile();

	/* Same format as the tray "Call Contact" menu: "Name  target",
	 * where target is the bare number for dialable contacts or the
	 * full sip: URI for foreign addresses. */
	for (int i = 0; i < contactEntries_.size(); ++i) {
		const ContactEntry &e = contactEntries_[i];
		QString target = uriToTarget(e.uri.toUtf8().constData());
		QString label = e.name.isEmpty()
			? target
			: QString("%1  %2").arg(e.name, target);

		auto *item = new QListWidgetItem();
		item->setData(Qt::UserRole, target);
		item->setData(Qt::UserRole + 1, i); /* entries_ index */
		historyList_->addItem(item);

		QWidget *row = makeRow(label, QIcon(), item);
		item->setSizeHint(row->sizeHint());
		historyList_->setItemWidget(item, row);
	}
}


/* ---- per-row action buttons + overlays ---------------------------- */

/** Small flat icon button used at the right end of a list row.
 *  No tooltip — tooltips can show through the overlays. */
static QToolButton *rowButton(const QString &iconName,
			      const QString &fallbackText,
			      QWidget *parent)
{
	auto *b = new QToolButton(parent);
	QIcon ic = QIcon::fromTheme(iconName);
	if (!ic.isNull())
		b->setIcon(ic);
	else
		b->setText(fallbackText);
	b->setAutoRaise(true);
	b->setFixedSize(22, 22);
	return b;
}


QWidget *CallDialog::makeRow(const QString &label, const QIcon &icon,
			     QListWidgetItem *item)
{
	auto *row = new QWidget(historyList_);
	auto *l = new QHBoxLayout(row);
	l->setContentsMargins(4, 0, 2, 0);
	l->setSpacing(4);

	if (!icon.isNull()) {
		auto *ic = new QLabel(row);
		ic->setPixmap(icon.pixmap(16, 16));
		ic->setAttribute(Qt::WA_TransparentForMouseEvents);
		l->addWidget(ic);
	}

	/* The label is transparent to mouse events so clicks on the
	 * text still reach the list item (click-to-fill and
	 * double-click-to-dial keep working). */
	auto *text = new QLabel(label, row);
	text->setAttribute(Qt::WA_TransparentForMouseEvents);
	l->addWidget(text, 1);

	if (showingContacts_) {
		QToolButton *edit = rowButton("document-edit", "\u270E", row);
		QToolButton *del  = rowButton("edit-delete", "\u2715", row);
		l->addWidget(edit);
		l->addWidget(del);

		connect(edit, &QToolButton::clicked, this, [this, item]() {
			openContactForm(item->data(Qt::UserRole + 1)
					.toInt());
		});
		connect(del, &QToolButton::clicked, this, [this, item]() {
			int idx = item->data(Qt::UserRole + 1).toInt();
			if (idx < 0 || idx >= contactEntries_.size())
				return;
			QString name = contactEntries_[idx].name;
			confirmOverlay(
				QString("Delete contact \"%1\"?")
					.arg(name.isEmpty()
						? contactEntries_[idx].uri
						: name),
				[this, idx]() {
					contactEntries_.removeAt(idx);
					saveContactsFile();
					refreshContacts();
				});
		});
	}
	else {
		QToolButton *add = rowButton("list-add", "+", row);
		QToolButton *del = rowButton("edit-delete", "\u2715", row);
		l->addWidget(add);
		l->addWidget(del);

		connect(add, &QToolButton::clicked, this, [this, item]() {
			openContactForm(-1, item);
		});
		connect(del, &QToolButton::clicked, this, [this, item]() {
			QDateTime ts = item->data(Qt::UserRole + 1)
				.toDateTime();
			QString num = item->data(Qt::UserRole + 2)
				.toString();
			QString uri = item->data(Qt::UserRole + 3)
				.toString();
			QString target = item->data(Qt::UserRole)
				.toString();
			confirmOverlay(
				QString("Delete \"%1\" from history?")
					.arg(target),
				[this, ts, num, uri]() {
					CallHistory::instance()
						->remove(ts, num, uri);
					refreshHistory();
				});
		});
	}

	return row;
}


void CallDialog::loadContactsFile()
{
	contactEntries_.clear();
	preservedLines_.clear();

	QFile f(contactsPath());
	if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		while (!f.atEnd()) {
			QString line = QString::fromUtf8(f.readLine());
			ContactEntry e;
			if (parseContactLine(line, e))
				contactEntries_.append(e);
			else
				preservedLines_.append(line);
		}
		f.close();
	}
}


/** Sort entries by name, write the contacts file and sync baresip's
 *  in-memory contact list on the re thread. */
void CallDialog::saveContactsFile()
{
	std::sort(contactEntries_.begin(), contactEntries_.end(),
		  [](const ContactEntry &a, const ContactEntry &b) {
		int c = a.name.compare(b.name, Qt::CaseInsensitive);
		if (c != 0)
			return c < 0;
		return uriToNumber(a.uri.toUtf8().constData())
			< uriToNumber(b.uri.toUtf8().constData());
	});

	QStringList lines;
	for (const ContactEntry &e : contactEntries_)
		lines << contactLine(e);

	QFile f(contactsPath());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate
		   | QIODevice::Text)) {
		QTextStream out(&f);
		for (const QString &l : preservedLines_)
			out << l;
		for (const QString &l : lines)
			out << l << "\n";
		out.flush();
		f.close();
	}

	/* Rebuild baresip's in-memory contact list (re thread). */
	qt_mod_sync_contacts(lines);

	emit contactsSaved();
}


void CallDialog::openContactForm(int index, QListWidgetItem *prefill)
{
	if (formOverlay_)
		formOverlay_->deleteLater();
	editingContact_ = index;

	formOverlay_ = new QFrame(panel_);
	{
		/* Same dialog style as the parent panel — themed
		 * palette(window) fill at ~84% opacity so it still
		 * reads as an overlay, mid-grey border, and
		 * borderless palette(text) labels (works on light
		 * and dark themes). */
		QColor bg = palette().color(QPalette::Window);
		formOverlay_->setStyleSheet(QString(
			"QFrame { background-color: rgba(%1,%2,%3,215);"
			"         border: 2px solid #808080;"
			"         border-radius: 8px; }"
			"QLabel { border: none; color: palette(text); }")
			.arg(bg.red()).arg(bg.green()).arg(bg.blue()));
	}

	/* Cover the panel with a small margin — 15% top, 10%
	 * bottom, 5% sides. */
	int mw = panel_->width() / 20;
	int mt = panel_->height() * 15 / 100;
	int mb = panel_->height() / 10;
	formOverlay_->setGeometry(mw, mt,
		panel_->width() - 2 * mw, panel_->height() - mt - mb);

	auto *lay = new QVBoxLayout(formOverlay_);
	lay->setSpacing(4); /* ~50% tighter than the default gap */

	auto *title = new QLabel(editingContact_ >= 0
		? "Edit Contact" : "Add Contact", formOverlay_);
	title->setStyleSheet("font-weight: bold; font-size: 15px;"
			     " border: none;");
	title->setAlignment(Qt::AlignCenter);
	lay->addWidget(title);

	auto *form = new QFormLayout();
	cNameEdit_ = new QLineEdit(formOverlay_);
	cNumEdit_  = new QLineEdit(formOverlay_);
	form->addRow("Name:",   cNameEdit_);
	form->addRow("Number:", cNumEdit_);
	lay->addLayout(form);

	auto *btnRow = new QHBoxLayout();
	btnRow->addStretch();
	auto *saveBtn = new QPushButton("Save Contact", formOverlay_);
	auto *backBtn = new QPushButton("Close", formOverlay_);
	btnRow->addWidget(saveBtn);
	btnRow->addWidget(backBtn);
	lay->addLayout(btnRow);

	if (index >= 0 && index < contactEntries_.size()) {
		const ContactEntry &e = contactEntries_[index];
		cNameEdit_->setText(e.name);
		cNumEdit_->setText(uriToTarget(e.uri.toUtf8().constData()));
	}
	else if (prefill) {
		/* Add-from-history: prefill with the entry's display
		 * name (if any) and dial target. */
		cNameEdit_->setText(prefill->data(Qt::UserRole + 4)
				    .toString());
		cNumEdit_->setText(prefill->data(Qt::UserRole).toString());
	}

	formOverlay_->show();
	formOverlay_->raise();

	connect(saveBtn, &QPushButton::clicked, this, [this]() {
		QString name   = cNameEdit_->text().trimmed();
		QString number = cNumEdit_->text().trimmed();
		bool uriInput = isUriInput(number);
		if (!uriInput && !validNumber(number)) {
			QMessageBox::warning(this, "Invalid number",
				"Enter a number or a sip: URI "
				"(min. 4 digits).");
			return;
		}

		if (editingContact_ >= 0
		    && editingContact_ < contactEntries_.size()) {
			ContactEntry &e = contactEntries_[editingContact_];
			QString oldNum = uriToNumber(
				e.uri.toUtf8().constData());
			e.name = name;
			if (uriInput)
				e.uri = uriToFull(
					number.toUtf8().constData());
			else if (number != oldNum)
				e.uri = completeUri(number,
						    uriHost(e.uri));
		}
		else {
			ContactEntry e;
			e.name = name;
			e.uri  = uriInput
				? uriToFull(number.toUtf8().constData())
				: completeUri(number, QString());
			contactEntries_.append(e);
		}

		saveContactsFile();
		refreshContacts();
		formOverlay_->deleteLater();
		formOverlay_ = nullptr;
	});

	connect(backBtn, &QPushButton::clicked, this, [this]() {
		formOverlay_->deleteLater();
		formOverlay_ = nullptr;
	});
}


void CallDialog::confirmOverlay(const QString &text,
				std::function<void()> onYes)
{
	if (deleteOverlay_)
		deleteOverlay_->deleteLater();
	pendingDelete_ = std::move(onYes);

	deleteOverlay_ = new QFrame(panel_);
	{
		/* Themed like the parent panel (palette(window) at
		 * ~78% opacity, mid-grey border) so it matches
		 * the active color scheme. */
		QColor bg = palette().color(QPalette::Window);
		deleteOverlay_->setStyleSheet(QString(
			"QFrame { background-color: rgba(%1,%2,%3,200);"
			"         border: 2px solid #808080;"
			"         border-radius: 8px; }"
			"QLabel { border: none; color: palette(text); }")
			.arg(bg.red()).arg(bg.green()).arg(bg.blue()));
	}

	/* 10% horizontal, 30% vertical margins — wide enough for
	 * the message to render on one line. */
	int mw = panel_->width() / 10;
	int mh = panel_->height() * 3 / 10;
	deleteOverlay_->setGeometry(mw, mh,
		panel_->width() - 2 * mw, panel_->height() - 2 * mh);

	auto *olay = new QVBoxLayout(deleteOverlay_);
	/* Stretches centre the content vertically; items fill
	 * the overlay width so the message text centres across
	 * the full width. */
	olay->addStretch();

	auto *msg = new QLabel(text, deleteOverlay_);
	msg->setStyleSheet("border: none; color: palette(text);"
			   " font-size: 14px; font-weight: bold;"
			   " padding-bottom: 1.0em;");
	msg->setAlignment(Qt::AlignCenter);
	msg->setWordWrap(true);
	olay->addWidget(msg);

	auto *btnRow = new QHBoxLayout();
	btnRow->setAlignment(Qt::AlignCenter);
	btnRow->setSpacing(20);

	auto *yesBtn = new QPushButton("Yes", deleteOverlay_);
	yesBtn->setStyleSheet(
		"QPushButton { background-color: #d32f2f; color: white;"
		"             border: none; border-radius: 4px;"
		"             padding: 6px 20px; font-weight: bold; }"
		"QPushButton:hover { background-color: #b71c1c; }");
	auto *noBtn = new QPushButton("No", deleteOverlay_);
	noBtn->setStyleSheet(
		"QPushButton { background-color: #424242; color: white;"
		"             border: none; border-radius: 4px;"
		"             padding: 6px 20px; font-weight: bold; }"
		"QPushButton:hover { background-color: #616161; }");
	btnRow->addWidget(yesBtn);
	btnRow->addWidget(noBtn);
	olay->addLayout(btnRow);
	olay->addStretch();

	deleteOverlay_->show();
	deleteOverlay_->raise();

	connect(yesBtn, &QPushButton::clicked, this, [this]() {
		if (pendingDelete_)
			pendingDelete_();
		pendingDelete_ = nullptr;
		if (deleteOverlay_)
			deleteOverlay_->deleteLater();
		deleteOverlay_ = nullptr;
	});
	connect(noBtn, &QPushButton::clicked, this, [this]() {
		pendingDelete_ = nullptr;
		if (deleteOverlay_)
			deleteOverlay_->deleteLater();
		deleteOverlay_ = nullptr;
	});
}


void CallDialog::resizeEvent(QResizeEvent *ev)
{
	QDialog::resizeEvent(ev);
	if (!panel_)
		return;
	if (formOverlay_) {
		int mw = panel_->width() / 20;
		int mt = panel_->height() * 15 / 100;
		int mb = panel_->height() / 10;
		formOverlay_->setGeometry(mw, mt,
			panel_->width() - 2 * mw,
			panel_->height() - mt - mb);
	}
	if (deleteOverlay_) {
		int mw = panel_->width() / 10;
		int mh = panel_->height() * 3 / 10;
		deleteOverlay_->setGeometry(mw, mh,
			panel_->width() - 2 * mw,
			panel_->height() - 2 * mh);
	}
}


void CallDialog::hideEvent(QHideEvent *ev)
{
	/* Drop any open overlays so a hidden-then-reshown panel
	 * always comes up clean. */
	if (formOverlay_) {
		formOverlay_->deleteLater();
		formOverlay_ = nullptr;
	}
	if (deleteOverlay_) {
		pendingDelete_ = nullptr;
		deleteOverlay_->deleteLater();
		deleteOverlay_ = nullptr;
	}
	QDialog::hideEvent(ev);
}
