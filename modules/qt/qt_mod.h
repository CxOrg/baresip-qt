/**
 * @file qt/qt_mod.h Qt UI module -- internal API
 *
 * Ported from the gtk module (Copyright (C) 2015 Charles E. Lehner,
 * Copyright (C) 2010 - 2015 Alfred E. Heggestad).
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMargins>
#include <QPoint>
#include <QSize>
#include <mutex>

extern "C" {
#include <re.h>
#include <rem.h>
#include <baresip.h>
}

/* baresip.h #defines "info" and "warning" as logging macros. Several
 * Qt classes (QMessageLogger, QMessageBox, ...) declare methods with
 * those exact names, and the macros would mangle those declarations
 * in any Qt header included afterwards. Undefine them here so every
 * file that includes qt_mod.h before other Qt headers is safe; use
 * BS_INFO()/BS_WARNING() below for baresip's own logging instead.
 */
#undef info
#undef warning
#define BS_INFO(...)    _info(false, __VA_ARGS__)
#define BS_WARNING(...) _warning(false, __VA_ARGS__)

#define CALL_INCOMING  0
#define CALL_OUTGOING  1
#define CALL_MISSED    2
#define CALL_REJECTED  3

class TrayApp;

/** Cross-thread work items posted from the Qt thread to the
 *  re/baresip main thread via mqueue.
 */
enum qt_mod_events {
	MQ_CONNECT,
	MQ_QUIT,
	MQ_ANSWER,
	MQ_HANGUP,
	MQ_SELECT_UA,
	MQ_DTMF,
	MQ_REGISTER,
	MQ_UNREGISTER,
	MQ_UA_ALLOC,
	MQ_UA_FREE,
	MQ_SYNC_CONTACTS,
};

struct qt_mod {
	thrd_t thread;
	bool run = false;
	struct mqueue *mq = nullptr;
	TrayApp *tray = nullptr;
	struct ua *ua_cur = nullptr;
	bool clean_number = false;
	/* Number passed via the "qtdial" command (tel: links). Stored
	 * under dial_mtx so it can be delivered to the tray once the
	 * Qt app is up (the -e fallback path can race startup). */
	std::mutex dial_mtx;
	QString pending_dial;
};

extern struct qt_mod qt_mod_obj;

/* Called from the Qt thread; pushes work onto the re thread. */
void qt_mod_connect(const char *uri);
void qt_mod_answer(struct call *call);
void qt_mod_hangup(struct call *call);
void qt_mod_select_ua(struct ua *ua);
void qt_mod_register(struct ua *ua);
void qt_mod_unregister(struct ua *ua);
void qt_mod_ua_alloc(const QString &line);
void qt_mod_ua_free(struct ua *ua);
void qt_mod_sync_contacts(const QStringList &lines);
void qt_mod_send_digit(struct call *call, char key);
void qt_mod_quit(void);

struct ua *qt_current_ua(void);

/** Strip a SIP URI down to the dial number ("sip:1234@host" -> "1234"). */
QString uriToNumber(const char *uri);

/** Account menu label: "<display name>  sip:<user>". */
QString accountLabel(struct ua *ua);

/** Layer-shell margins for the popup panels. `anchorPos` (e.g. the
 *  tray-icon click position) selects the screen; the margin is the
 *  tray panel's height + 16px on the screen edge holding the tray
 *  (top or bottom), and 8px on the horizontal edge nearest the
 *  anchor's half of the screen. `anchorLeft`/`anchorBottom` report
 *  the chosen edges. */
QMargins qtPanelMargins(const QPoint &anchorPos, bool *anchorLeft,
			bool *anchorBottom);

class QWindow;
/** Apply top + nearest-horizontal-edge anchors and the computed
 *  margins to a layer-shell window. No-op without HAVE_LAYERSHELL
 *  or when the window has no layer-shell surface. */
void qtPanelApplyAnchors(QWindow *win, const QPoint &anchorPos);
