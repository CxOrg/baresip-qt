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

	case BEVENT_REGISTERING:
	case BEVENT_UNREGISTERING:
	case BEVENT_REGISTER_OK:
	case BEVENT_REGISTER_FAIL:
		QMetaObject::invokeMethod(mod->tray, "accountStatus",
			Qt::QueuedConnection,
			Q_ARG(quintptr, reinterpret_cast<quintptr>(ua)),
			Q_ARG(QString, accountLabel(ua)),
			Q_ARG(QString, QString::fromUtf8(event_reg_str(ev))));
		break;

	case BEVENT_CALL_INCOMING:
		/* Log incoming calls at ringing, even if they are
		 * rejected/hangup before being answered. Mirrors the
		 * outgoing-call logging at BEVENT_CALL_OUTGOING.
		 * Store just the phone number; the full SIP URI is
		 * reconstructed on dialing. */
		QMetaObject::invokeMethod(mod->tray, "addHistory",
			Qt::QueuedConnection,
			Q_ARG(QString, uriToNumber(call_peeruri(call))),
			Q_ARG(int, CALL_INCOMING),
			Q_ARG(QString,
				QString::fromUtf8(call_peername(call))));

		QMetaObject::invokeMethod(mod->tray, "callIncoming",
			Qt::QueuedConnection,
			Q_ARG(quintptr, reinterpret_cast<quintptr>(call)),
			Q_ARG(QString, uriToNumber(call_peeruri(call))),
			Q_ARG(QString, QString::fromUtf8(call_peername(call))));
		break;

	case BEVENT_CALL_OUTGOING:
		/* Log dialed numbers immediately, even if the call never
		 * connects. Store just the phone number; the full SIP URI
		 * is reconstructed on dialing. */
		QMetaObject::invokeMethod(mod->tray, "addHistory",
			Qt::QueuedConnection,
			Q_ARG(QString, uriToNumber(call_peeruri(call))),
			Q_ARG(int, CALL_OUTGOING),
			Q_ARG(QString,
				QString::fromUtf8(call_peername(call))));
		break;

	case BEVENT_CALL_CLOSED:
	{
		bool missed = !call_is_outgoing(call)
			&& call_state(call) != CALL_STATE_TERMINATED
			&& call_state(call) != CALL_STATE_ESTABLISHED;

		/* Update the call history with the actual duration
		 * for connected calls. */
		if (!missed) {
			uint32_t dur = call_duration(call);
			if (dur > 0) {
				QMetaObject::invokeMethod(mod->tray,
					"updateHistoryDuration",
					Qt::QueuedConnection,
					Q_ARG(QString,
						uriToNumber(call_peeruri(call))),
					Q_ARG(uint, dur));
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

	case MQ_REGISTER:
		ua_register(static_cast<struct ua *>(data));
		break;

	case MQ_UNREGISTER:
		ua_unregister(static_cast<struct ua *>(data));
		break;

	case MQ_UA_ALLOC: {
		char *aor = static_cast<char *>(data);
		struct ua *newua = NULL;
		err = ua_alloc(&newua, aor);
		if (err)
			BS_WARNING("qt: failed to create account: %m\n", err);
		mem_deref(data);
		break;
	}

	case MQ_UA_FREE: {
		struct ua *ua = static_cast<struct ua *>(data);
		if (qt_mod_obj.ua_cur == ua)
			qt_mod_obj.ua_cur = NULL;
		mem_deref(ua);
		break;
	}
	}
}


void qt_mod_connect(const char *uri)
{
	char *uric = NULL;
	struct pl url_pl;
	int err;

	pl_set_str(&url_pl, uri);

	err = account_uri_complete_strdup(ua_account(qt_current_ua()),
					   &uric, &url_pl);
	if (err)
		return;

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


void qt_mod_register(struct ua *ua)
{
	mqueue_push(qt_mod_obj.mq, MQ_REGISTER, ua);
}


void qt_mod_unregister(struct ua *ua)
{
	mqueue_push(qt_mod_obj.mq, MQ_UNREGISTER, ua);
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

	app.exec();

	mod->run = false;
	bevent_unregister(event_handler);
	mod->tray = nullptr;

	return 0;
}


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


static int module_init(void)
{
	int err;

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
