/**
 * @file qt/qt_mod.cpp Qt UI module
 *
 * Ported from the gtk module (Copyright (C) 2015 Charles E. Lehner,
 * Copyright (C) 2010 - 2015 Alfred E. Heggestad).
 *
 * Creates a system tray icon (shown as a StatusNotifierItem applet on
 * KDE Plasma and other SNI-aware desktops) with a menu for making and
 * receiving calls.
 */
#include "qt_mod.h"
#include "tray_app.h"

#include <QApplication>
#include <QMetaObject>
#include <QString>
#include <QFile>
#include <QDir>
#include <QList>
#include <QScreen>
#include <QDebug>
#include <QLoggingCategory>
#include <QGuiApplication>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusReply>

#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#ifdef HAVE_LAYERSHELL
#include <LayerShellQt/Window>
#endif
#ifdef HAVE_KSTYLE
#include <kstylemanager.h>
#endif


/** Extract the user part (phone number) from a SIP URI for history.
 *  "sip:+441234567890@domain;transport=udp" -> "+441234567890"
 *  "sip:bob@example.com"                     -> "bob"
 *  "+441234567890"                           -> "+441234567890"
 *  The full SIP URI is reconstructed on dialing via
 *  account_uri_complete_strdup().
 */
QString uriToNumber(const char *uri)
{
	QString s = QString::fromUtf8(uri).trimmed();

	if (s.startsWith("sip:", Qt::CaseInsensitive))
		s = s.mid(4);
	else if (s.startsWith("sips:", Qt::CaseInsensitive))
		s = s.mid(5);

	int semi = s.indexOf(';');
	if (semi >= 0)
		s = s.left(semi);

	int at = s.indexOf('@');
	if (at >= 0)
		s = s.left(at);

	return s.trimmed();
}


bool isDialNumber(const QString &s)
{
	bool digit = false;
	for (const QChar &c : s) {
		if (c.isDigit()) {
			digit = true;
			continue;
		}
		if (c != '+' && c != '*' && c != '#' && c != ' ' &&
		    c != '-' && c != '(' && c != ')' && c != '.')
			return false;
	}
	return digit;
}


QString uriToFull(const char *uri)
{
	QString s = QString::fromUtf8(uri).trimmed();

	int lt = s.indexOf('<');
	int gt = s.indexOf('>');
	if (lt >= 0 && gt > lt)
		s = s.mid(lt + 1, gt - lt - 1);

	if (!s.startsWith("sip:", Qt::CaseInsensitive) &&
	    !s.startsWith("sips:", Qt::CaseInsensitive))
		s.prepend("sip:");

	int semi = s.indexOf(';');
	if (semi >= 0)
		s = s.left(semi);

	return s;
}


/** Host part of a URI ("sip:u@host:port;p" -> "host"). */
static QString uriHostPart(QString s)
{
	int lt = s.indexOf('<');
	int gt = s.indexOf('>');
	if (lt >= 0 && gt > lt)
		s = s.mid(lt + 1, gt - lt - 1);
	if (s.startsWith("sip:", Qt::CaseInsensitive))
		s = s.mid(4);
	else if (s.startsWith("sips:", Qt::CaseInsensitive))
		s = s.mid(5);
	int at = s.indexOf('@');
	if (at < 0)
		return QString();
	s = s.mid(at + 1);
	int end = s.indexOf(';');
	if (end >= 0)
		s = s.left(end);
	end = s.indexOf(':');
	if (end >= 0)
		s = s.left(end);
	return s.trimmed();
}


/** True when uri's host differs from ua account's domain. */
static bool isForeignToUa(struct ua *ua, const char *uri)
{
	QString host = uriHostPart(QString::fromUtf8(uri));
	if (host.isEmpty() || !ua)
		return false;
	QString ours = uriHostPart(
		QString::fromUtf8(account_aor(ua_account(ua))));
	return !ours.isEmpty() &&
		host.compare(ours, Qt::CaseInsensitive) != 0;
}


bool isForeignUri(const char *uri)
{
	return isForeignToUa(qt_current_ua(), uri);
}


QString uriToTarget(const char *uri)
{
	QString num = uriToNumber(uri);
	if (isDialNumber(num) && !isForeignUri(uri))
		return num;
	return uriToFull(uri);
}


/** Account menu label: "<display name>  sip:<user>" — the
 *  @domain part of the AOR is suppressed. Returns just
 *  "sip:<user>" when the account has no display name.
 */
QString accountLabel(struct ua *ua)
{
	const struct account *acc = ua_account(ua);
	QString user = uriToNumber(account_aor(acc));
	QString base = QString("sip:%1").arg(user);

	const char *dn = account_display_name(acc);
	QString name = dn
		? QString::fromUtf8(dn).remove('"').trimmed()
		: QString();

	return name.isEmpty() ? base : QString("%1  %2").arg(name, base);
}


/** Height of the Plasma panel holding the system tray, queried
 *  from plasmashell's evaluateScript() D-Bus API. Needed because
 *  dodge/autohide panels don't reserve an available-geometry strut.
 *  `isBottom` reports whether that panel sits on the bottom screen
 *  edge, `centerX` the tray widget's horizontal centre (-1 when
 *  unknown). Returns 0 when plasmashell is unreachable or reports
 *  nothing. */
static int queryTrayPanel(QScreen *screen, bool *isBottom, int *centerX)
{
	QDBusInterface iface(QStringLiteral("org.kde.plasmashell"),
		QStringLiteral("/PlasmaShell"),
		QStringLiteral("org.kde.PlasmaShell"),
		QDBusConnection::sessionBus());
	if (!iface.isValid())
		return 0;

	/* evaluateScript() returns whatever the script prints. Emit
	 * "screenIndex:location:height:centerX" for each panel
	 * containing the org.kde.plasma.systemtray widget; centerX is
	 * the tray widget's horizontal centre in screen coordinates
	 * (widget.geometry is screen-relative). */
	static const char script[] =
		"for (var i=0;i<panelIds.length;i++){"
		"var p=panelById(panelIds[i]);"
		"var ws=p.widgets(); var t=false; var g=null;"
		"for (var j=0;j<ws.length;j++)"
		"if (ws[j].type==='org.kde.plasma.systemtray')"
		"{t=true;g=ws[j].geometry;break;}"
		"if (t) print(p.screen+':'+p.location+':'+p.height+':'+"
		"(g?Math.round(g.x+g.width/2):-1));}";

	QDBusReply<QString> r = iface.call(QStringLiteral("evaluateScript"),
					   QLatin1String(script));
	if (!r.isValid())
		return 0;

	/* Prefer a panel on the anchor's screen (panel.screen == -1
	 * means unassigned); otherwise any tray panel. */
	int target = screen ? QGuiApplication::screens().indexOf(screen) : 0;
	int hAny = 0, hScr = 0, cxAny = -1, cxScr = -1;
	bool bAny = false, bScr = false;
	for (const QString &line : r.value().split('\n', Qt::SkipEmptyParts)) {
		QStringList kv = line.trimmed().split(':');
		if (kv.size() != 4)
			continue;
		int scr = kv[0].toInt(), h = kv[2].toInt(), cx = kv[3].toInt();
		bool bottom = (kv[1] == QLatin1String("bottom"));
		if (h > hAny) {
			hAny = h;
			bAny = bottom;
			cxAny = cx;
		}
		if ((scr == target || scr == -1) && h > hScr) {
			hScr = h;
			bScr = bottom;
			cxScr = cx;
		}
	}

	if (isBottom)
		*isBottom = hScr > 0 ? bScr : bAny;
	if (centerX)
		*centerX = hScr > 0 ? cxScr : cxAny;
	return hScr > 0 ? hScr : hAny;
}


QMargins qtPanelMargins(const QPoint &anchorPos, bool *anchorLeft,
			bool *anchorBottom)
{
	/* Screen containing the anchor point (tray icon position),
	 * else the primary screen. */
	QScreen *screen = anchorPos.isNull()
		? nullptr
		: QGuiApplication::screenAt(anchorPos);
	if (!screen)
		screen = QGuiApplication::primaryScreen();

	QRect full  = screen ? screen->geometry() : QRect(0,0,1920,1080);
	QRect avail = screen ? screen->availableGeometry() : full;

	/* Vertical margin = tray-panel height + 16px gap, applied on
	 * the edge the tray panel sits on. The geometry strut covers
	 * reserve-space panels; the D-Bus query covers dodge/autohide
	 * panels which leave no strut. The query also returns the tray
	 * widget's centre-x in screen coordinates. */
	bool bottom = false;
	int trayCx = -1;
	int trayH = queryTrayPanel(screen, &bottom, &trayCx);
	if (anchorBottom)
		*anchorBottom = bottom;

	/* Anchor to the screen edge on the tray's half of the screen.
	 * Prefer the tray widget's real position from D-Bus -- under
	 * Wayland QCursor::pos() can't be queried, so anchorPos is a
	 * fallback only (X11 or first-open before any click). */
	bool left;
	if (trayCx >= 0)
		left = trayCx < full.center().x();
	else
		left = !anchorPos.isNull() &&
			anchorPos.x() < full.center().x();
	if (anchorLeft)
		*anchorLeft = left;
	int side = left ? 8 : 0, other = left ? 0 : 8;

	if (bottom) {
		int b = qMax(full.bottom() - avail.bottom(), trayH) + 16;
		return QMargins(side, 0, other, b);
	}
	int t = qMax(avail.top() - full.top(), trayH) + 16;
	return QMargins(side, t, other, 0);
}


void qtPanelApplyAnchors(QWindow *win, const QPoint &anchorPos)
{
#ifdef HAVE_LAYERSHELL
	if (!win)
		return;
	auto *ls = LayerShellQt::Window::get(win);
	if (!ls)
		return;

	bool left = false, bottom = false;
	QMargins m = qtPanelMargins(anchorPos, &left, &bottom);
	ls->setAnchors(LayerShellQt::Window::Anchors(
		(bottom ? LayerShellQt::Window::AnchorBottom
			: LayerShellQt::Window::AnchorTop) |
		(left ? LayerShellQt::Window::AnchorLeft
		      : LayerShellQt::Window::AnchorRight)));
	ls->setMargins(m);
#else
	Q_UNUSED(win);
	Q_UNUSED(anchorPos);
#endif
}


/**
 * @defgroup qt_mod qt_mod
 *
 * Qt Menu-based User-Interface module
 *
 * Creates a tray icon with a menu for making calls.
 */


struct qt_mod qt_mod_obj;

static void event_handler(enum bevent_ev ev, struct bevent *event,
			   void *arg);


struct ua *qt_current_ua(void)
{
	if (qt_mod_obj.ua_cur)
		return qt_mod_obj.ua_cur;

	/* No explicit selection (Account submenu): prefer a registered
	 * UA so disabled accounts aren't used for dialing. Not cached —
	 * registration state changes at runtime. */
	struct le *le;
	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = static_cast<struct ua *>(le->data);
		if (ua_isregistered(ua))
			return ua;
	}

	return static_cast<struct ua *>(list_ledata(
					list_head(uag_list())));
}


static const char *event_reg_str(enum bevent_ev ev)
{
	switch (ev) {

	case BEVENT_REGISTERING:   return "registering";
	case BEVENT_REGISTER_OK:   return "OK";
	case BEVENT_REGISTER_FAIL: return "ERR";
	case BEVENT_UNREGISTERING: return "unregistering";
	default: return "?";
	}
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct qt_mod *mod = static_cast<struct qt_mod *>(arg);
	struct ua   *ua   = bevent_get_ua(event);
	struct call *call = bevent_get_call(event);

	if (!mod->tray)
		return;

	switch (ev) {

	case BEVENT_CREATE:
		/* A UA was created by any means (settings apply, /uanew)
		 * -- rebuild the account menu. */
		QMetaObject::invokeMethod(mod->tray, "accountsChanged",
			Qt::QueuedConnection);
		break;

	case BEVENT_REGISTERING:
	case BEVENT_UNREGISTERING:
	case BEVENT_REGISTER_OK:
	case BEVENT_REGISTER_FAIL:
		QMetaObject::invokeMethod(mod->tray, "accountStatus",
			Qt::QueuedConnection,
			Q_ARG(quintptr, reinterpret_cast<quintptr>(ua)),
			Q_ARG(QString, accountLabel(ua)),
			Q_ARG(QString, QString::fromUtf8(event_reg_str(ev))));
		if (ev == BEVENT_REGISTER_OK)
			QMetaObject::invokeMethod(mod->tray, "publishPresence",
				Qt::QueuedConnection,
				Q_ARG(quintptr,
					reinterpret_cast<quintptr>(ua)));
		break;

	case BEVENT_CALL_INCOMING:
	{
		/* Log incoming calls at ringing, even if they are
		 * rejected/hangup before being answered. Mirrors the
		 * outgoing-call logging at BEVENT_CALL_OUTGOING.
		 * History stores the dial number (when the user part
		 * is a phone number) and the full SIP URI. The number
		 * is kept regardless of the originating host — incoming
		 * calls from a PSTN gateway have a foreign host but the
		 * user part is still a valid dial number. */
		QString peerNum = uriToNumber(call_peeruri(call));
		if (!isDialNumber(peerNum))
			peerNum.clear();
		QString peerFull = uriToFull(call_peeruri(call));

		QMetaObject::invokeMethod(mod->tray, "addHistory",
			Qt::QueuedConnection,
			Q_ARG(QString, peerNum),
			Q_ARG(QString, peerFull),
			Q_ARG(int, CALL_INCOMING),
			Q_ARG(QString,
				QString::fromUtf8(call_peername(call))));

		QMetaObject::invokeMethod(mod->tray, "callIncoming",
			Qt::QueuedConnection,
			Q_ARG(quintptr, reinterpret_cast<quintptr>(call)),
			Q_ARG(QString,
				peerNum.isEmpty() ? peerFull : peerNum),
			Q_ARG(QString, QString::fromUtf8(call_peername(call))));
		break;
	}

	case BEVENT_CALL_OUTGOING:
	{
		QString peerNum = uriToNumber(call_peeruri(call));
		if (!isDialNumber(peerNum))
			peerNum.clear();
		QString peerFull = uriToFull(call_peeruri(call));

		QMetaObject::invokeMethod(mod->tray, "addHistory",
			Qt::QueuedConnection,
			Q_ARG(QString, peerNum),
			Q_ARG(QString, peerFull),
			Q_ARG(int, CALL_OUTGOING),
			Q_ARG(QString,
				QString::fromUtf8(call_peername(call))));
		break;
	}

	case BEVENT_CALL_CLOSED:
	{
		bool missed = !call_is_outgoing(call)
			&& call_state(call) != CALL_STATE_TERMINATED
			&& call_state(call) != CALL_STATE_ESTABLISHED;

		/* Update the call history with the actual duration
		 * for connected calls. */
		if (!missed) {
			uint32_t dur = call_duration(call);
			QString peerFull = uriToFull(call_peeruri(call));
			int callType = call_is_outgoing(call)
				? CALL_OUTGOING : CALL_INCOMING;
			if (dur > 0) {
				QMetaObject::invokeMethod(mod->tray,
					"updateHistoryDuration",
					Qt::QueuedConnection,
					Q_ARG(QString, peerFull),
					Q_ARG(uint, dur),
					Q_ARG(int, callType));
			}
		}

		QMetaObject::invokeMethod(mod->tray, "callClosed",
			Qt::QueuedConnection,
			Q_ARG(quintptr, reinterpret_cast<quintptr>(call)),
			Q_ARG(bool, missed),
			Q_ARG(QString, QString::fromUtf8(call_peeruri(call))),
			Q_ARG(QString, QString::fromUtf8(call_peername(call))));
		break;
	}

	case BEVENT_CALL_ESTABLISHED:
		/* Incoming calls are already logged at BEVENT_CALL_INCOMING
		 * (ringing) so they appear in history even if rejected.
		 * Outgoing calls are logged at BEVENT_CALL_OUTGOING. */
		QMetaObject::invokeMethod(mod->tray, "callEstablished",
			Qt::QueuedConnection,
			Q_ARG(quintptr, reinterpret_cast<quintptr>(call)));
		break;

	default:
		break;
	}
}


/* Small heap-allocated bundle for pushing a (call, digit) pair through
 * mqueue in one void* payload. Allocated with plain new/delete -- not
 * a libre mem_alloc() object, so it must be freed with delete, not
 * mem_deref(), in the handler below.
 */
struct dtmf_data {
	struct call *call;
	char key;
};


static void mqueue_handler(int id, void *data, void *arg)
{
	struct qt_mod *mod = static_cast<struct qt_mod *>(arg);
	struct ua *ua = qt_current_ua();
	struct call *call;
	char *uri;
	int err;

	switch (static_cast<enum qt_mod_events>(id)) {

	case MQ_CONNECT:
		uri = static_cast<char *>(data);
		err = ua_connect(ua, &call, NULL, uri, VIDMODE_ON);

		if (mod->tray) {
			/* History is now recorded at the event level
			 * (BEVENT_CALL_ESTABLISHED) so all calls are
			 * logged regardless of how they were initiated. */
			if (!err && call) {
				QMetaObject::invokeMethod(mod->tray,
					"callOutgoing", Qt::QueuedConnection,
					Q_ARG(quintptr,
						reinterpret_cast<quintptr>(call)),
					Q_ARG(QString, QString::fromUtf8(uri)));
			}

			if (err) {
				QMetaObject::invokeMethod(mod->tray,
					"showWarning", Qt::QueuedConnection,
					Q_ARG(QString, "Call failed"),
					Q_ARG(QString,
						QString("Connecting to \"%1\" "
							"failed (%2).")
						.arg(QString::fromUtf8(uri))
						.arg(err)));
			}
		}

		mem_deref(data);
		break;

	case MQ_ANSWER:
		call = static_cast<struct call *>(data);
		err = ua_answer(ua, call, VIDMODE_ON);

		if (mod->tray) {
			/* History is now recorded at the event level
			 * (BEVENT_CALL_ESTABLISHED). */
			if (err) {
				QMetaObject::invokeMethod(mod->tray,
					"showWarning", Qt::QueuedConnection,
					Q_ARG(QString, "Call failed"),
					Q_ARG(QString,
						QString("Answering the call "
							"failed (%1).")
						.arg(err)));
			}
		}
		break;

	case MQ_HANGUP:
		call = static_cast<struct call *>(data);
		ua_hangup(ua, call, 0, NULL);
		break;

	case MQ_QUIT:
		ua_stop_all(false);
		break;

	case MQ_DTMF: {
		struct dtmf_data *dd = static_cast<struct dtmf_data *>(data);
		call_send_digit(dd->call, dd->key);
		delete dd;
		break;
	}

	case MQ_SELECT_UA:
		qt_mod_obj.ua_cur = static_cast<struct ua *>(data);
		break;

	case MQ_UA_ALLOC: {
		char *aor = static_cast<char *>(data);
		struct ua *newua = NULL;
		err = ua_alloc(&newua, aor);
		if (err)
			BS_WARNING("qt: failed to create account: %m\n", err);
		mem_deref(data);
		/* The UA list changed -- rebuild the account menu. */
		if (mod->tray)
			QMetaObject::invokeMethod(mod->tray, "accountsChanged",
				Qt::QueuedConnection);
		break;
	}

	case MQ_UA_FREE: {
		struct ua *ua = static_cast<struct ua *>(data);
		if (qt_mod_obj.ua_cur == ua)
			qt_mod_obj.ua_cur = NULL;
		mem_deref(ua);
		/* The UA list changed -- rebuild the account menu. */
		if (mod->tray)
			QMetaObject::invokeMethod(mod->tray, "accountsChanged",
				Qt::QueuedConnection);
		break;
	}

	case MQ_SYNC_CONTACTS: {
		/* Rebuild the in-memory contact list to match the
		 * contacts file just written by the contacts panel. */
		QStringList *lines = static_cast<QStringList *>(data);
		struct contacts *cs = baresip_contacts();
		if (cs) {
			struct list *lst = contact_list(cs);
			struct le *le = list_head(lst);
			while (le) {
				struct le *next = le->next;
				contact_remove(cs,
					static_cast<struct contact *>(le->data));
				le = next;
			}
			for (const QString &l : *lines) {
				QByteArray utf8 = l.toUtf8();
				struct pl pl;
				pl.p = utf8.constData();
				pl.l = (size_t)utf8.size();
				(void)contact_add(cs, NULL, &pl);
			}
		}
		delete lines;
		break;
	}
	}
}


void qt_mod_connect(const char *uri)
{
	char *uric = NULL;
	struct pl url_pl;
	int err;

	QString in = QString::fromUtf8(uri).trimmed();

	/* qt_clean_number: strip -/()/spaces from plain numbers.
	 * clean_number() leaves input unchanged if it contains
	 * letters or '@', so SIP URIs pass through unharmed. */
	if (qt_mod_obj.clean_number) {
		QByteArray ba = in.toUtf8();
		if (clean_number(ba.data()) >= 0)
			in = QString::fromUtf8(ba);
	}

	if (in.contains('@') || in.startsWith("sip:", Qt::CaseInsensitive)
	    || in.startsWith("sips:", Qt::CaseInsensitive)) {
		/* A full SIP URI (e.g. a foreign-domain address from
		 * history or contacts) is dialed directly — account
		 * domain completion must not rewrite it. */
		if (!in.startsWith("sip:", Qt::CaseInsensitive) &&
		    !in.startsWith("sips:", Qt::CaseInsensitive))
			in.prepend("sip:");
		if (str_dup(&uric, in.toUtf8().constData()))
			return;
	}
	else {
		/* Bare number: complete it with the selected
		 * account's domain. */
		pl_set_str(&url_pl, uri);
		err = account_uri_complete_strdup(
			ua_account(qt_current_ua()), &uric, &url_pl);
		if (err)
			return;
	}

	mqueue_push(qt_mod_obj.mq, MQ_CONNECT, uric);
}


void qt_mod_answer(struct call *call)
{
	mqueue_push(qt_mod_obj.mq, MQ_ANSWER, call);
}


void qt_mod_hangup(struct call *call)
{
	mqueue_push(qt_mod_obj.mq, MQ_HANGUP, call);
}


void qt_mod_select_ua(struct ua *ua)
{
	mqueue_push(qt_mod_obj.mq, MQ_SELECT_UA, ua);
}


void qt_mod_ua_alloc(const QString &line)
{
	/* Recreate a UA from an accounts-file line (re-enabling an
	 * account that was disabled at startup). ua_alloc registers
	 * the account itself when regint > 0. */
	char *buf = NULL;
	if (str_dup(&buf, line.trimmed().toUtf8().constData()))
		return;
	mqueue_push(qt_mod_obj.mq, MQ_UA_ALLOC, buf);
}


void qt_mod_ua_free(struct ua *ua)
{
	mqueue_push(qt_mod_obj.mq, MQ_UA_FREE, ua);
}


void qt_mod_sync_contacts(const QStringList &lines)
{
	mqueue_push(qt_mod_obj.mq, MQ_SYNC_CONTACTS,
		    new QStringList(lines));
}


void qt_mod_send_digit(struct call *call, char key)
{
	struct dtmf_data *dd = new dtmf_data{call, key};
	mqueue_push(qt_mod_obj.mq, MQ_DTMF, dd);
}


void qt_mod_quit(void)
{
	mqueue_push(qt_mod_obj.mq, MQ_QUIT, 0);
}


static int qt_thread(void *arg)
{
	struct qt_mod *mod = static_cast<struct qt_mod *>(arg);

	static char appName[] = "baresip";
	static char *qargv[] = { appName, nullptr };
	int qargc = 1;

	QApplication app(qargc, qargv);
	app.setApplicationName("baresip");
	app.setQuitOnLastWindowClosed(false);

	/* KWindowSystem (pulled in by KF6ConfigWidgets/KStyleManager)
	 * can't load its platform plugin when QApplication runs on a
	 * non-main thread — it falls back to a no-op implementation,
	 * but logs a warning on every panel show. Suppress that
	 * category to keep the console clean.
	 *
	 * Also suppress kf.kio.widgets.kdirmodel — a known KF6/KIO
	 * race-condition bug in KDirModel that spams "No node found
	 * for item that was just removed" when QFileDialog populates
	 * its file listing under KDE Plasma. */
	QLoggingCategory::setFilterRules(
		"kf.windowsystem=false\n"
		"kf.kio.widgets.kdirmodel=false");

#ifdef HAVE_KSTYLE
	/* Apply the user's configured KDE widget style (Breeze by default)
	 * so the call panel adopts Plasma panel styling. */
	KStyleManager::initStyle();
#endif

	TrayApp tray(mod);
	mod->tray = &tray;
	tray.show();

	BS_INFO("qt: tray applet starting\n");

	bevent_register(event_handler, mod);
	mod->run = true;

	/* Deliver a dial number that arrived before the tray was up
	 * (baresip -e "qtdial <num>" fallback path). */
	{
		QString pending;
		{
			std::lock_guard<std::mutex> lk(mod->dial_mtx);
			pending = mod->pending_dial;
			mod->pending_dial.clear();
		}
		if (!pending.isEmpty())
			tray.openDialNumber(pending);
	}

	app.exec();

	mod->run = false;
	bevent_unregister(event_handler);
	mod->tray = nullptr;

	return 0;
}


/** "qtdial" command — used by baresip-qt-handler for tel: links.
 *  Opens the dial panel with the number populated; the user confirms
 *  with the green button. Runs on the re thread (ctrl_tcp / -e). */
static int qtdial_handler(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = static_cast<const struct cmd_arg *>(arg);
	(void)pf;

	if (!str_isset(carg->prm))
		return EINVAL;

	/* tel: links carry a dial number; sip: URIs keep their full
	 * address so foreign domains survive to the dial field. */
	QString number = uriToNumber(carg->prm);
	if (!isDialNumber(number))
		number = uriToFull(carg->prm);
	if (number.isEmpty())
		return EINVAL;

	/* Stash the number, then queue delivery. If the Qt app isn't
	 * up yet (baresip -e "qtdial ..." fallback can run before the
	 * qt thread creates QApplication) qt_thread delivers it after
	 * the tray is created. Otherwise the lambda runs inside
	 * app.exec(), by which time mod->tray is always set. */
	struct qt_mod *mod = &qt_mod_obj;
	{
		std::lock_guard<std::mutex> lk(mod->dial_mtx);
		mod->pending_dial = number;
	}
	if (qApp) {
		QMetaObject::invokeMethod(qApp, []() {
			struct qt_mod *m = &qt_mod_obj;
			QString n;
			{
				std::lock_guard<std::mutex> lk(m->dial_mtx);
				n = m->pending_dial;
				m->pending_dial.clear();
			}
			if (!n.isEmpty() && m->tray)
				m->tray->openDialNumber(n);
		}, Qt::QueuedConnection);
	}
	return 0;
}


static const struct cmd qt_cmdv[] = {
	{"qtdial", 0, CMD_PRM, "Open dial panel with number", qtdial_handler},
};


/** Destroy accounts marked ";enabled=no" in ~/.baresip/accounts.
 *  Runs at module_init — ua_init has already populated uag_list, so
 *  the Nth non-comment line maps to the Nth UA. mem_deref() deletes
 *  the UA entirely (same as the /uadel command): it is unlinked from
 *  uag_list, never registers, and doesn't appear in the Account menu.
 *  The settings panel still shows the line by parsing the file.
 */
static void apply_enabled_accounts(void)
{
	QFile f(QDir::homePath() + "/.baresip/accounts");
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return;

	QList<bool> enabled;
	while (!f.atEnd()) {
		QString line = QString::fromUtf8(f.readLine());
		QString t = line.trimmed();
		if (t.isEmpty() || t.startsWith('#'))
			continue;
		enabled.append(!line.contains(";enabled=no"));
	}
	f.close();

	int i = 0;
	struct le *le = list_head(uag_list());
	while (le) {
		/* mem_deref() unlinks the node — fetch next first. */
		struct le *next = le->next;
		struct ua *ua = static_cast<struct ua *>(le->data);
		if (i < enabled.size() && !enabled[i]) {
			BS_INFO("qt: account %d disabled, "
				"not loading\n", i + 1);
			mem_deref(ua);
		}
		le = next;
		++i;
	}
}


/** Single-instance guard: refuse to start if another baresip process
 *  is already running. Uses a PID file in ~/.baresip/baresip.pid; a
 *  stale file (dead PID) is removed. Runs at module_init on the
 *  baresip thread. Returns true if another instance is running.
 */
static bool another_instance_running(void)
{
	QString path = QDir::homePath() + "/.baresip/baresip.pid";
	QFile f(path);
	if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		bool ok = false;
		pid_t oldpid = f.readLine().trimmed().toLong(&ok);
		f.close();
		if (ok && oldpid > 0 && kill(oldpid, 0) == 0) {
			BS_INFO("qt: already running (pid %d)\n",
				(int)oldpid);
			return true;
		}
		QFile::remove(path);
	}

	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate |
		   QIODevice::Text)) {
		f.write(QString("%1\n").arg(getpid()).toUtf8());
		f.close();
	}
	return false;
}


/** Migrate an existing ~/.baresip/config for the Qt build: enable the
 *  qt and ctrl_tcp app modules, disable gtk/echo, comment out the
 *  deprecated jitter-buffer keys, and add missing keys. Runs at
 *  module_init — after baresip has parsed the config, so changes take
 *  effect on the next start.
 */
static void migrate_config(void)
{
	QString path = QDir::homePath() + "/.baresip/config";
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return;

	QStringList out;
	bool changed = false;
	bool qt_app = false, ctrl_app = false;
	bool ctrl_listen = false, qt_clean = false;

	while (!f.atEnd()) {
		QString line = QString::fromUtf8(f.readLine());
		/* Keep the newline; chomp only for parsing. */
		QString eol = line.endsWith("\r\n") ? "\r\n"
			    : (line.endsWith("\n") ? "\n" : QString());
		QString body = line.left(line.size() - eol.size());

		QString t = body;
		bool commented = t.trimmed().startsWith('#');
		QString key = t.section(' ', 0, 0,
			QString::SectionSkipEmpty);
		QString val = t.section(' ', 1, 1,
			QString::SectionSkipEmpty);

		QString emitLine;
		if (key == "module_app") {
			QString mod = val.section('.', 0, 0);
			if (mod == "gtk" && !commented) {
				emitLine = "#module_app\t\tgtk" MOD_EXT;
			}
			else if (mod == "qt") {
				qt_app = true;
				if (commented)
					emitLine = "module_app\t\tqt" MOD_EXT;
			}
			else if (mod == "echo" && !commented) {
				emitLine = "#module_app\t\techo" MOD_EXT;
			}
			else if (mod == "ctrl_tcp") {
				ctrl_app = true;
				if (commented)
					emitLine = "module_app\t\t"
						"ctrl_tcp" MOD_EXT;
			}
		}
		else if (key.startsWith("audio_jitter_buffer") ||
			 key.startsWith("video_jitter_buffer")) {
			if (!commented)
				emitLine = "#" + body;
		}
		else if (key == "ctrl_tcp_listen") {
			ctrl_listen = true;
			if (commented)
				emitLine = "ctrl_tcp_listen\t\t127.0.0.1:4444";
		}
		else if (key == "qt_clean_number") {
			qt_clean = true;
			if (commented)
				emitLine = "qt_clean_number\tyes";
		}

		if (!emitLine.isNull()) {
			out << emitLine + eol;
			changed = true;
		}
		else
			out << line;
	}
	f.close();

	if (!qt_app) {
		out << "module_app\t\tqt" MOD_EXT "\n";
		changed = true;
	}
	if (!ctrl_app) {
		out << "module_app\t\tctrl_tcp" MOD_EXT "\n";
		changed = true;
	}
	if (!ctrl_listen) {
		out << "ctrl_tcp_listen\t\t127.0.0.1:4444\n";
		changed = true;
	}
	if (!qt_clean) {
		out << "qt_clean_number\tyes\n";
		changed = true;
	}

	if (changed && f.open(QIODevice::WriteOnly | QIODevice::Truncate |
			      QIODevice::Text)) {
		for (const QString &l : out)
			f.write(l.toUtf8());
		f.close();
		BS_INFO("qt: migrated %s for the Qt build\n",
			path.toUtf8().constData());
	}
}


/** Seed ~/.baresip/contacts with phone-number-oriented example
 *  entries on first install (file absent). The upstream template's
 *  generic SIP examples don't match the Qt UI's typed contacts. */
static void seed_contacts(void)
{
	QString path = QDir::homePath() + "/.baresip/contacts";
	if (QFile::exists(path))
		return;

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
		return;

	f.write(
		"#\n"
		"# SIP contacts\n"
		"#\n"
		"# Displayname <sip:user@domain>;addr-params\n"
		"#\n"
		"#  addr-params:\n"
		"#    ;presence={none,p2p}\n"
		"#    ;access={allow,block}\n"
		"#    ;audio={inactive,sendonly,recvonly,sendrecv}\n"
		"#    ;video={inactive,sendonly,recvonly,sendrecv}\n"
		"#    ;type={Primary,Work,Home,Mobile,Business,SIP,Fax,Other}\n"
		"\n"
		"\n"
		"\"Example Contact\" <sip:1234567890@example.com>;type=Primary\n"
		"\"Work Phone\" <sip:9876543210@example.com>;type=Work\n"
		"\"Mobile User\" <sip:+441234567890@example.com>;type=Mobile\n"
		"\n"
		"# Access rules\n"
		"#\"Catch All\" <sip:*@*>;access=block\n"
		"#\"Good Friend\" <sip:good@example.com>;"
		"access=allow\n"
		"\n");
	f.close();
}


static int module_init(void)
{
	int err;

	/* Single-instance guard: the PID lock lives here (not in
	 * main()) so the package can build against unmodified
	 * upstream baresip. */
	if (another_instance_running())
		return EALREADY;

	/* Fix up an existing config for the Qt build and seed the
	 * contacts file on first install. */
	migrate_config();
	seed_contacts();

	qt_mod_obj.clean_number = false;
	conf_get_bool(conf_cur(), "qt_clean_number", &qt_mod_obj.clean_number);

	apply_enabled_accounts();

	err = mqueue_alloc(&qt_mod_obj.mq, mqueue_handler, &qt_mod_obj);
	if (err)
		return err;

	err = thread_create_name(&qt_mod_obj.thread, "qt", qt_thread,
				  &qt_mod_obj);
	if (err)
		return err;

	err = cmd_register(baresip_commands(), qt_cmdv,
			   RE_ARRAY_SIZE(qt_cmdv));
	if (err)
		return err;

	return 0;
}


static int module_close(void)
{
	if (qt_mod_obj.run) {
		QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
	}

	if (qt_mod_obj.thread)
		thrd_join(qt_mod_obj.thread, NULL);

	qt_mod_obj.mq = static_cast<struct mqueue *>(mem_deref(qt_mod_obj.mq));

	bevent_unregister(event_handler);
	cmd_unregister(baresip_commands(), qt_cmdv);

	return 0;
}


extern "C" {
/* NB: in C++ (unlike C), a `const` object at namespace scope has
 * internal linkage by default -- even inside an extern "C" block,
 * which only controls name mangling/calling convention, not this.
 * Without an explicit `extern` here, exports_qt would never be
 * visible outside this translation unit, and static.c's reference
 * to it would fail to link.
 */
extern EXPORT_SYM const struct mod_export DECL_EXPORTS(qt) = {
	"qt",
	"application",
	module_init,
	module_close,
};
}
