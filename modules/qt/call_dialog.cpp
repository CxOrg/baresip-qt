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
#include <QTabBar>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QSettings>
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

/** Parse a contacts-file line into name/type/uri/params. Handles
 *  `"Name" <sip:user@host>;type=Work;params`, bare `<sip:...>` and
 *  plain `sip:...` lines. Returns false for comments/blank lines.
 *  The type is stored as a `;type=...` URI parameter; extracted
 *  into `e.type` and stripped from `e.params`. */
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

	/* Extract ;type=... from params into e.type. */
	e.type.clear();
	if (!e.params.isEmpty()) {
		QStringList parts = e.params.split(';',
			Qt::SkipEmptyParts);
		QStringList rest;
		for (const QString &p : parts) {
			QString pt = p.trimmed();
			if (pt.startsWith("type=", Qt::CaseInsensitive))
				e.type = pt.mid(5).trimmed();
			else
				rest.append(pt);
		}
		e.params = rest.isEmpty()
			? QString()
			: ";" + rest.join(";");
	}
	return true;
}

/** Render a ContactEntry back to a contacts-file line. The type
 *  is stored as a `;type=...` URI parameter. */
static QString contactLine(const CallDialog::ContactEntry &e)
{
	QString line;
	if (!e.name.isEmpty())
		line = QString("\"%1\" ").arg(e.name);
	line += "<" + e.uri + ">";
	QString params = e.params;
	if (!e.type.isEmpty()) {
		QString tp = "type=" + e.type;
		params = params.isEmpty() ? ";" + tp
					   : params + ";" + tp;
	}
	if (!params.isEmpty())
		line += params;
	return line;
}

/** Parse a CSV line, handling quoted fields with embedded commas
 *  and doubled-quote escapes ("" -> "). */
static QStringList parseCsvLine(const QString &line)
{
	QStringList fields;
	QString field;
	bool inQuotes = false;
	for (int i = 0; i < line.size(); ++i) {
		QChar c = line[i];
		if (inQuotes) {
			if (c == '"') {
				if (i + 1 < line.size()
				    && line[i + 1] == '"') {
					field += '"';
					++i;
				}
				else
					inQuotes = false;
			}
			else
				field += c;
		}
		else {
			if (c == '"')
				inQuotes = true;
			else if (c == ',') {
				fields.append(field);
				field.clear();
			}
			else
				field += c;
		}
	}
	fields.append(field);
	return fields;
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
		"         border: 1px solid #808080;"
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
		"         border: 1px solid #808080;"
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
	/* Widen the dialog to fit the table contents, but never
	 * wider than the available screen area. */
	if (!historyList_ || historyList_->rowCount() == 0)
		return;

	int maxWidth = 0;
	for (int i = 0; i < historyList_->columnCount(); ++i)
		maxWidth += historyList_->columnWidth(i);

	/* Add space for the vertical scrollbar and frame margins. */
	maxWidth += style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 8;

	/* Don't shrink below the initial width (buttons + label). */
	if (maxWidth < 470)
		maxWidth = 470;

	/* Cap at the available screen width so the panel stays
	 * fully visible. */
	QRect avail = screen()->availableGeometry();
	int maxAvail = avail.width() - 40;
	if (maxWidth > maxAvail)
		maxWidth = maxAvail;

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

	/* Top row: label + tab strip switching the list between the
	 * call history and the contacts list. */
	auto *topRow = new QHBoxLayout();
	auto *label = new QLabel(panel);
	label->setText("Enter SIP URI or number:");
	topRow->addWidget(label);
	topRow->addStretch();
	listTabs_ = new QTabBar(panel);
	listTabs_->addTab("History");
	listTabs_->addTab("Contact");
	listTabs_->setExpanding(false);
	topRow->addWidget(listTabs_);
	layout->addLayout(topRow);
	connect(listTabs_, &QTabBar::currentChanged,
		this, [this](int idx) {
		showingContacts_ = (idx == 1);
		refreshList();
	});

	uriEdit_ = new QLineEdit(panel);
	uriEdit_->setAlignment(Qt::AlignLeft);
	layout->addWidget(uriEdit_);

	/* In-call DTMF dialpad (embedded, hidden unless InCall). */
	buildDialpad(panel);
	layout->addWidget(dialpadWidget_);

	/* Call history list (shown only in Dialing state). */
	historyList_ = new QTableWidget(panel);
	historyList_->setMaximumHeight(200);
	historyList_->setMinimumHeight(60);
	historyList_->setShowGrid(false);
	historyList_->setSelectionBehavior(
		QAbstractItemView::SelectRows);
	historyList_->setSelectionMode(
		QAbstractItemView::SingleSelection);
	historyList_->setVerticalScrollMode(
		QAbstractItemView::ScrollPerPixel);
	historyList_->verticalHeader()->setVisible(false);
	historyList_->setVerticalScrollBarPolicy(
		Qt::ScrollBarAsNeeded);
	/* Disable horizontal scrollbar — the dialog widens to fit. */
	historyList_->setHorizontalScrollBarPolicy(
		Qt::ScrollBarAlwaysOff);
	/* Header font: 1pt smaller than the table font. */
	QFont hf = historyList_->font();
	hf.setPointSize(hf.pointSize() - 1);
	historyList_->horizontalHeader()->setFont(hf);
	historyList_->horizontalHeader()->setStyleSheet(
		"QHeaderView { margin: 0px; padding: 0px; }"
		"QHeaderView::section { padding: 0px 0px 1px 4px; "
		"margin: 0px; border: none; "
		"background: palette(window); text-align: left; }");
	historyList_->horizontalHeader()->setDefaultAlignment(
		Qt::AlignLeft | Qt::AlignVCenter);
	/* Allow user to drag column widths. */
	historyList_->horizontalHeader()->setSectionsMovable(false);
	historyList_->horizontalHeader()->setSectionResizeMode(
		QHeaderView::Interactive);
	/* No right padding on cell text; no padding on icon/count
	 * columns (0 and 1). */
	historyList_->setStyleSheet(
		"QTableWidget::item { padding-right: 0px; }"
		"QTableWidget::item:selected { padding-right: 0px; }");
	layout->addWidget(historyList_);
	connect(historyList_, &QTableWidget::itemClicked,
		this, &CallDialog::onHistoryClicked);
	connect(historyList_, &QTableWidget::itemDoubleClicked,
		this, &CallDialog::onHistoryDoubleClicked);
	/* Save column widths when the user resizes. */
	connect(historyList_->horizontalHeader(),
		&QHeaderView::sectionResized,
		this, [this](int col, int, int w) {
			QSettings s;
			s.beginGroup(showingContacts_
				? "contactCols" : "historyCols");
			s.setValue(QString::number(col), w);
		});

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
		dialpadWidget_->hide();
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
		dialpadWidget_->hide();
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
		/* DTMF dialpad only for outgoing calls (navigating IVR
		 * systems); not needed for incoming calls. */
		dialpadWidget_->setVisible(isOutgoing_);
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


void CallDialog::buildDialpad(QWidget *parent)
{
	dialpadWidget_ = new QWidget(parent);
	dialpadWidget_->hide();

	auto *vlay = new QVBoxLayout(dialpadWidget_);
	vlay->setContentsMargins(0, 0, 0, 0);

	/* 5% padding above the dialpad (panel ~320px → ~16px). */
	vlay->addSpacing(16);

	dialpadLog_ = new QLineEdit(dialpadWidget_);
	dialpadLog_->setReadOnly(true);
	dialpadLog_->setAlignment(Qt::AlignRight);
	dialpadLog_->setPlaceholderText("Keys sent this session");
	vlay->addWidget(dialpadLog_);

	/* Grid container centred at 80% panel width. */
	auto *gridWrap = new QHBoxLayout();
	gridWrap->setContentsMargins(0, 0, 0, 0);
	gridWrap->addStretch(1);

	auto *grid = new QGridLayout();
	grid->setSpacing(4);
	static const char *keys[4][3] = {
		{ "1", "2", "3" },
		{ "4", "5", "6" },
		{ "7", "8", "9" },
		{ "*", "0", "#" },
	};
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 3; col++) {
			const char *label = keys[row][col];
			auto *btn = new QPushButton(label, dialpadWidget_);
			btn->setMinimumSize(44, 44);
			char key = label[0];
			connect(btn, &QPushButton::clicked,
				this, [this, key]() { sendDigit(key); });
			grid->addWidget(btn, row, col);
		}
	}
	gridWrap->addLayout(grid, 8);
	gridWrap->addStretch(1);
	vlay->addLayout(gridWrap);

	/* 5% padding below the dialpad (panel ~320px → ~16px). */
	vlay->addSpacing(16);
}


void CallDialog::sendDigit(char key)
{
	qt_mod_send_digit(reinterpret_cast<struct call *>(callPtr_), key);
	if (dialpadLog_)
		dialpadLog_->setText(dialpadLog_->text() + QChar(key));
}


void CallDialog::refreshHistory()
{
	if (!historyList_)
		return;

	historyList_->clear();
	historyList_->setColumnCount(6);
	historyList_->setHorizontalHeaderLabels(
		{"", "X", "Call Number/URI", "Date/Time", "M:S", ""});
	/* Generic call icon in the call-direction column header. */
	historyList_->horizontalHeaderItem(0)->setIcon(
		QIcon::fromTheme("call-start-symbolic"));

	/* Restore saved column widths for non-stretch columns. */
	QSettings s;
	s.beginGroup("historyCols");
	int w0 = s.value("0", -1).toInt();   /* icon */
	int w1 = s.value("1", -1).toInt();   /* count */
	int w2 = s.value("2", -1).toInt();   /* number — stretch */
	int w3 = s.value("3", -1).toInt();   /* duration */
	int w4 = s.value("4", -1).toInt();   /* date/time */
	int w5 = s.value("5", -1).toInt();   /* actions */
	s.endGroup();

	/* Most recent first; show up to 10 entries. */
	auto entries = CallHistory::instance()->recent(10);
	/* Display newest at top. */
	int row = 0;
	for (int i = entries.size() - 1; i >= 0; --i, ++row) {
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
		QString display = e.info.isEmpty() ? target : e.info;

		QString countStr = QString::number(e.count);
		QString durStr;
		if (e.duration > 0) {
			int mins = e.duration / 60;
			int secs = e.duration % 60;
			durStr = QString("%1:%2")
				.arg(mins)
				.arg(secs, 2, 10, QChar('0'));
		}
		QString dateStr = e.ts.toString("MM-dd hh:mm");

		historyList_->insertRow(row);

		/* Icon column (col 0) */
		auto *iconItem = new QTableWidgetItem();
		QIcon ic = QIcon::fromTheme(iconName);
		if (ic.isNull() && !fallback.isEmpty())
			ic = QIcon::fromTheme(fallback);
		iconItem->setIcon(ic);
		historyList_->setItem(row, 0, iconItem);

		/* Count column (col 1) */
		auto *cntItem = new QTableWidgetItem(countStr);
		cntItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		historyList_->setItem(row, 1, cntItem);

		/* Number column ("Call #") — stash target in
		 * UserRole for click-to-fill. */
		auto *numItem = new QTableWidgetItem(display);
		numItem->setData(Qt::UserRole,     target);
		numItem->setData(Qt::UserRole + 1, e.ts);
		numItem->setData(Qt::UserRole + 2, e.number);
		numItem->setData(Qt::UserRole + 3, e.uri);
		numItem->setData(Qt::UserRole + 4, e.info);
		numItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		historyList_->setItem(row, 2, numItem);

		/* Duration column ("m:s") — now col 4 */
		auto *durItem = new QTableWidgetItem(durStr);
		durItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		historyList_->setItem(row, 4, durItem);

		/* Date/Time column — now col 3 */
		auto *dateItem = new QTableWidgetItem(dateStr);
		dateItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		historyList_->setItem(row, 3, dateItem);

		/* Actions column */
		historyList_->setCellWidget(row, 5,
			makeActionWidget(row, false));
	}

	/* Auto-fit all columns to content, then restore any
	 * saved widths. Call # (col 2) stretches to fill. */
	for (int c = 0; c < 6; ++c) {
		if (c == 2) continue;
		historyList_->resizeColumnToContents(c);
	}
	/* Apply saved widths if the user has resized. */
	if (w0 >= 0) historyList_->setColumnWidth(0, w0);
	if (w1 >= 0) historyList_->setColumnWidth(1, w1);
	if (w3 >= 0) historyList_->setColumnWidth(3, w3);
	if (w4 >= 0) historyList_->setColumnWidth(4, w4);
	if (w5 >= 0) historyList_->setColumnWidth(5, w5);
	/* Call # stretches, or uses saved width. */
	if (w2 > 0)
		historyList_->setColumnWidth(2, w2);
	else
		historyList_->horizontalHeader()->
			setSectionResizeMode(2, QHeaderView::Stretch);
}


void CallDialog::onHistoryClicked(QTableWidgetItem *item)
{
	if (!item)
		return;
	/* Target is stashed on the Number column (col 2) in history
	 * view, and on the Name column (col 0) in contacts view. */
	int row = item->row();
	int col = showingContacts_ ? 0 : 2;
	auto *numItem = historyList_->item(row, col);
	if (!numItem)
		return;
	QString target = numItem->data(Qt::UserRole).toString();
	if (!target.isEmpty())
		uriEdit_->setText(target);
}


void CallDialog::onHistoryDoubleClicked(QTableWidgetItem *item)
{
	if (!item)
		return;
	int row = item->row();
	int col = showingContacts_ ? 0 : 2;
	auto *numItem = historyList_->item(row, col);
	if (!numItem)
		return;
	QString target = numItem->data(Qt::UserRole).toString();
	if (target.isEmpty())
		return;
	uriEdit_->setText(target);
	/* Double-click = dial immediately. */
	onGreen();
}


void CallDialog::refreshList()
{
	if (showingContacts_)
		refreshContacts();
	else
		refreshHistory();
}


void CallDialog::showContacts(bool contacts)
{
	showingContacts_ = contacts;
	if (listTabs_)
		listTabs_->setCurrentIndex(contacts ? 1 : 0);
	refreshList();
}


void CallDialog::refreshContacts()
{
	if (!historyList_)
		return;

	historyList_->clear();
	historyList_->setColumnCount(4);
	historyList_->setHorizontalHeaderLabels(
		{"Name", "Number/URI", "Type", ""});

	/* Restore saved column widths. */
	QSettings s;
	s.beginGroup("contactCols");
	int c0 = s.value("0", -1).toInt();    /* name */
	int c1 = s.value("1", -1).toInt();    /* number */
	int c2 = s.value("2", -1).toInt();    /* type */
	int c3 = s.value("3", -1).toInt();    /* actions */
	s.endGroup();

	loadContactsFile();

	for (int i = 0; i < contactEntries_.size(); ++i) {
		const ContactEntry &e = contactEntries_[i];
		QString target = uriToTarget(e.uri.toUtf8().constData());

		int row = i;
		historyList_->insertRow(row);

		auto *nameItem = new QTableWidgetItem(e.name);
		nameItem->setData(Qt::UserRole, target);
		nameItem->setData(Qt::UserRole + 1, i);
		nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		historyList_->setItem(row, 0, nameItem);

		auto *numItem = new QTableWidgetItem(target);
		numItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		historyList_->setItem(row, 1, numItem);

		auto *typeItem = new QTableWidgetItem(e.type);
		typeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		historyList_->setItem(row, 2, typeItem);

		historyList_->setCellWidget(row, 3,
			makeActionWidget(row, true));
	}

	/* Auto-fit all columns to content, then restore saved
	 * widths. Name and Number/URI stretch to fill. */
	for (int c = 0; c < 4; ++c) {
		if (c == 0 || c == 1) continue;
		historyList_->resizeColumnToContents(c);
	}
	if (c2 >= 0) historyList_->setColumnWidth(2, c2);
	if (c3 >= 0) historyList_->setColumnWidth(3, c3);
	if (c0 > 0)
		historyList_->setColumnWidth(0, c0);
	else
		historyList_->horizontalHeader()->
			setSectionResizeMode(0, QHeaderView::Stretch);
	if (c1 > 0)
		historyList_->setColumnWidth(1, c1);
	else
		historyList_->horizontalHeader()->
			setSectionResizeMode(1, QHeaderView::Stretch);
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


QWidget *CallDialog::makeActionWidget(int row, bool isContact)
{
	auto *w = new QWidget(historyList_);
	auto *l = new QHBoxLayout(w);
	l->setContentsMargins(2, 0, 2, 0);
	l->setSpacing(2);

	if (isContact) {
		QToolButton *edit = rowButton("document-edit", "\u270E", w);
		QToolButton *del  = rowButton("edit-delete", "\u2715", w);
		l->addWidget(edit);
		l->addWidget(del);

		connect(edit, &QToolButton::clicked, this, [this, row]() {
			auto *item = historyList_->item(row, 0);
			if (item)
				openContactForm(item->data(Qt::UserRole + 1)
						.toInt());
		});
		connect(del, &QToolButton::clicked, this, [this, row]() {
			auto *item = historyList_->item(row, 0);
			if (!item)
				return;
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
		QToolButton *add = rowButton("list-add", "+", w);
		QToolButton *del = rowButton("edit-delete", "\u2715", w);
		l->addWidget(add);
		l->addWidget(del);

		connect(add, &QToolButton::clicked, this, [this, row]() {
			auto *item = historyList_->item(row, 2);
			openContactForm(-1, item);
		});
		connect(del, &QToolButton::clicked, this, [this, row]() {
			auto *item = historyList_->item(row, 2);
			if (!item)
				return;
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

	return w;
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


void CallDialog::openContactForm(int index, QTableWidgetItem *prefill)
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
			"QFrame { background-color: rgba(%1,%2,%3,230);"
			"         border: 1px solid #808080;"
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
	cTypeEdit_ = new QComboBox(formOverlay_);
	cTypeEdit_->setEditable(true);
	cTypeEdit_->addItems(QStringList()
		<< "Primary" << "Work" << "Home" << "Mobile"
		<< "Business" << "SIP" << "Fax" << "Other");
	cNumEdit_  = new QLineEdit(formOverlay_);
	form->addRow("Name:", cNameEdit_);
	form->addRow("Type:",  cTypeEdit_);
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
		cTypeEdit_->setEditText(e.type);
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
			e.type = cTypeEdit_->currentText().trimmed();
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
			e.type = cTypeEdit_->currentText().trimmed();
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
				std::function<void()> onYes,
				const QString &noText)
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
			"QFrame { background-color: rgba(%1,%2,%3,230);"
			"         border: 1px solid #808080;"
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
	auto *noBtn = new QPushButton(noText, deleteOverlay_);
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


void CallDialog::showImportInstructions()
{
	if (importOverlay_)
		importOverlay_->deleteLater();

	importOverlay_ = new QFrame(panel_);
	{
		QColor bg = palette().color(QPalette::Window);
		importOverlay_->setStyleSheet(QString(
			"QFrame { background-color: rgba(%1,%2,%3,230);"
			"         border: 1px solid #808080;"
			"         border-radius: 8px; }"
			"QLabel { border: none; color: palette(text); }")
			.arg(bg.red()).arg(bg.green()).arg(bg.blue()));
	}

	/* Cover 90% of the panel — 5% margin all sides. */
	int mw = panel_->width() / 20;
	int mh = panel_->height() / 20;
	importOverlay_->setGeometry(mw, mh,
		panel_->width() - 2 * mw,
		panel_->height() - 2 * mh);

	auto *lay = new QVBoxLayout(importOverlay_);
	lay->setSpacing(8);

	auto *title = new QLabel("Note: CSV columns are imported as follows.",
				 importOverlay_);
	title->setStyleSheet("font-weight: bold; font-size: 16px;"
			     " border: none;");
	title->setWordWrap(true);
	title->setAlignment(Qt::AlignLeft);
	lay->addWidget(title);

	auto *note = new QLabel(
		"<ol style=\"margin-left: 5px;\">"
		"<li>Multiple Name columns are concatenated to a single "
		"Name column.</li>"
		"<li>Multiple Phone columns Home Phone, Mobile Phone, "
		"Business Phone, SIP Phone etc create multiple records "
		"with Home, Mobile, Business, SIP as type.</li>"
		"<li>The number or URI is imported as is.</li>"
		"<li>All other columns are ignored.</li>"
		"</ol>",
		importOverlay_);
	note->setStyleSheet("border: none; font-size: 17px;");
	note->setWordWrap(true);
	lay->addWidget(note, 1);

	auto *btnRow = new QHBoxLayout();
	btnRow->setAlignment(Qt::AlignCenter);
	btnRow->setSpacing(20);

	auto *importBtn = new QPushButton("Import", importOverlay_);
	importBtn->setStyleSheet(
		"QPushButton { background-color: #2e7d32; color: white;"
		"             border: none; border-radius: 4px;"
		"             padding: 6px 20px; font-weight: bold; }"
		"QPushButton:hover { background-color: #1b5e20; }");
	auto *cancelBtn = new QPushButton("Cancel", importOverlay_);
	cancelBtn->setStyleSheet(
		"QPushButton { background-color: #424242; color: white;"
		"             border: none; border-radius: 4px;"
		"             padding: 6px 20px; font-weight: bold; }"
		"QPushButton:hover { background-color: #616161; }");
	btnRow->addWidget(importBtn);
	btnRow->addWidget(cancelBtn);
	lay->addLayout(btnRow);

	importOverlay_->show();
	importOverlay_->raise();

	connect(importBtn, &QPushButton::clicked, this, [this]() {
		/* Keep the import overlay visible while the file browser
		 * is open so the dial panel doesn't lose focus and hide.
		 * The panel is re-shown after the dialog closes. */
		QString path = QFileDialog::getOpenFileName(
			this, "Import Contacts CSV", QString(),
			"CSV Files (*.csv)");
		if (importOverlay_) {
			importOverlay_->deleteLater();
			importOverlay_ = nullptr;
		}
		if (path.isEmpty())
			return;
		/* Re-show the panel in case it was hidden by the
		 * file dialog grabbing focus. */
		show();
		raise();
		activateWindow();
		importCsv(path);
	});
	connect(cancelBtn, &QPushButton::clicked, this, [this]() {
		if (importOverlay_)
			importOverlay_->deleteLater();
		importOverlay_ = nullptr;
	});
}


void CallDialog::importCsv(const QString &path)
{
	/* Parse the CSV file: header row identifies name/phone columns.
	 * Each data row's name columns are concatenated; each populated
	 * phone column becomes a separate ContactEntry with the column
	 * title (minus "phone") as the type. */
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QMessageBox::warning(this, "Import failed",
			"Could not open the CSV file.");
		return;
	}

	QTextStream in(&f);
	QString headerLine = in.readLine();
	if (headerLine.isNull()) {
		QMessageBox::warning(this, "Import failed",
			"The CSV file is empty.");
		return;
	}

	QStringList headers = parseCsvLine(headerLine);

	/* Identify name columns (header contains "name") and phone
	 * columns (header contains "phone"). The type is the phone
	 * column title with "phone" removed. */
	QList<int> nameCols, phoneCols;
	QStringList phoneTypes;
	for (int i = 0; i < headers.size(); ++i) {
		QString h = headers[i].toLower();
		if (h.contains("name"))
			nameCols.append(i);
		if (h.contains("phone")) {
			phoneCols.append(i);
			QString type = headers[i];
			type.remove("phone", Qt::CaseInsensitive);
			type = type.trimmed();
			phoneTypes.append(type);
		}
	}

	if (phoneCols.isEmpty()) {
		QMessageBox::warning(this, "Import failed",
			"No phone columns found in the CSV header.");
		return;
	}

	/* Parse data rows and build imported records. */
	QList<ContactEntry> imported;
	QSet<QString> uniqueNames;
	while (!in.atEnd()) {
		QString line = in.readLine();
		if (line.trimmed().isEmpty())
			continue;
		QStringList fields = parseCsvLine(line);

		/* Concatenate all name column values into one Name. */
		QString name;
		for (int col : nameCols) {
			if (col < fields.size()) {
				QString part = fields[col].trimmed();
				if (!part.isEmpty()) {
					if (!name.isEmpty())
						name += " ";
					name += part;
				}
			}
		}
		if (name.isEmpty())
			continue;

		/* Each populated phone column → one record. Skip
		 * the row entirely if no phone column is populated. */
		bool anyPhone = false;
		for (int i = 0; i < phoneCols.size(); ++i) {
			int col = phoneCols[i];
			if (col >= fields.size())
				continue;
			QString number = fields[col].trimmed();
			if (number.isEmpty())
				continue;
			anyPhone = true;

			ContactEntry e;
			e.name = name;
			e.type = phoneTypes[i].isEmpty()
				? "Primary" : phoneTypes[i];
			/* If the number is already a sip: URI, use it
			 * directly; otherwise complete it via the
			 * current account's domain. */
			if (isUriInput(number))
				e.uri = uriToFull(
					number.toUtf8().constData());
			else
				e.uri = completeUri(number, QString());
			imported.append(e);
		}
		if (anyPhone)
			uniqueNames.insert(name);
	}

	if (imported.isEmpty()) {
		QMessageBox::information(this, "Import",
			"No phone records found in the CSV file.");
		return;
	}

	/* Show the confirmation overlay. On Yes, merge imported
	 * records with existing contacts, sort by name then type,
	 * and write the contacts file. */
	QString msg = QString("%1 phone records found for %2 names. "
			      "Add to Contacts?")
		.arg(imported.size()).arg(uniqueNames.size());

	confirmOverlay(msg, [this, imported]() {
		/* Merge: append imported records to existing. */
		for (const ContactEntry &e : imported)
			contactEntries_.append(e);

		/* Sort by name, then by type. */
		std::sort(contactEntries_.begin(),
			  contactEntries_.end(),
			  [](const ContactEntry &a, const ContactEntry &b) {
			int c = a.name.compare(b.name,
					       Qt::CaseInsensitive);
			if (c != 0)
				return c < 0;
			return a.type.compare(b.type,
					      Qt::CaseInsensitive) < 0;
		});

		saveContactsFile();
		refreshContacts();
	}, "Cancel");
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
	if (importOverlay_) {
		int mw = panel_->width() / 20;
		int mh = panel_->height() / 20;
		importOverlay_->setGeometry(mw, mh,
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
	if (importOverlay_) {
		importOverlay_->deleteLater();
		importOverlay_ = nullptr;
	}
	QDialog::hideEvent(ev);
}
