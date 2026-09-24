/**
 * @file qt/tray_app.cpp Qt UI module -- tray icon + menu
 */
#include "tray_app.h"
#include "call_dialog.h"
#include "call_history.h"
#include "settings_dialog.h"

#include <QMessageBox>
#include <QInputDialog>
#include <QIcon>
#include <QPainter>
#include <QDateTime>
#include <QApplication>
#include <QCursor>
#include <QFile>
#include <QDir>


TrayApp::TrayApp(struct qt_mod *mod, QObject *parent)
	: QObject(parent), mod_(mod)
{
	trayIcon_ = new QSystemTrayIcon(this);
	setTrayIcon("call-start", QString());
	trayIcon_->setToolTip("baresip");

	connect(trayIcon_, &QSystemTrayIcon::activated,
		this, &TrayApp::onTrayActivated);

	buildMenu();

	trayIcon_->setContextMenu(menu_);
}


TrayApp::~TrayApp()
{
	delete idleCallDialog_;
	delete settingsDialog_;
}


void TrayApp::show()
{
	trayIcon_->show();
}


void TrayApp::setTrayIcon(const QString &themeName, const QString &fallback)
{
	QIcon icon = QIcon::fromTheme(themeName);
	if (icon.isNull() && !fallback.isEmpty())
		icon = QIcon::fromTheme(fallback);
	if (icon.isNull())
		icon = QIcon::fromTheme("call-start");

	trayIcon_->setIcon(icon);
}


void TrayApp::refreshTrayMenu()
{
	/* Under KDE Plasma, the tray menu isn't a native popup -- it's
	 * exported over D-Bus (StatusNotifierItem/DBusMenu), and that
	 * export can miss mutations made to an already-registered menu
	 * (adding actions to an existing submenu, swapping Accept/Reject
	 * for Hang Up, etc). Re-registering forces a full re-export.
	 */
	trayIcon_->setContextMenu(nullptr);
	trayIcon_->setContextMenu(menu_);
}


/** Path to the persisted presence state (~/.baresip/presence). */
static QString presencePath()
{
	QString home = QString::fromLocal8Bit(qgetenv("HOME"));
	if (home.isEmpty())
		home = QDir::homePath();
	return home + "/.baresip/presence";
}


/** Small filled-circle icon used as the presence status spot. */
static QIcon spotIcon(const QColor &color)
{
	QPixmap pm(12, 12);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawEllipse(1, 1, 10, 10);
	return QIcon(pm);
}


void TrayApp::buildMenu()
{
	menu_ = new QMenu();

	/* User Presence: a single toggle with a green/red spot.
	 * Restores the last chosen state (default: available) and
	 * applies it to every loaded account — baresip's own
	 * default is CLOSED, so always push the state. */
	presenceAct_ = menu_->addAction("User Presence");
	connect(presenceAct_, &QAction::triggered,
		this, &TrayApp::onPresenceToggled);
	presenceOpen_ = true;
	{
		QFile f(presencePath());
		if (f.open(QIODevice::ReadOnly | QIODevice::Text))
			presenceOpen_ = QString::fromUtf8(f.readAll())
				.trimmed() != "closed";
	}
	for (struct le *le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = static_cast<struct ua *>(le->data);
		ua_presence_status_set(ua,
			presenceOpen_ ? PRESENCE_OPEN : PRESENCE_CLOSED);
	}
	presenceAct_->setIcon(spotIcon(presenceOpen_
		? QColor("#4caf50") : QColor("#e53935")));

	/* Online accounts submenu */
	accountsMenu_ = menu_->addMenu("Online Accounts");
	accountsGroup_ = new QActionGroup(this);
	accountsGroup_->setExclusive(true);
	connect(accountsGroup_, &QActionGroup::triggered,
		this, &TrayApp::onAccountToggled);
	populateAccounts();

	menu_->addSeparator();

	/* Dial */
	QAction *dialAct = menu_->addAction("Call/Dial ...");
	connect(dialAct, &QAction::triggered, this, &TrayApp::onDial);

	/* Call history — direct link to the dial panel showing the
	 * history list. */
	QAction *histAct = menu_->addAction("Call History ...");
	connect(histAct, &QAction::triggered, this, [this]() {
		onDial();
		if (idleCallDialog_)
			idleCallDialog_->showContacts(false);
	});

	/* Contacts — direct link to the dial panel showing the
	 * contacts list. */
	QAction *contactAct = menu_->addAction("Call Contact ...");
	connect(contactAct, &QAction::triggered, this, [this]() {
		onDial();
		if (idleCallDialog_)
			idleCallDialog_->showContacts(true);
	});

	menu_->addSeparator();

	QAction *settingsAct = menu_->addAction("Settings ...");
	connect(settingsAct, &QAction::triggered, this, &TrayApp::onSettings);

	/* Import contacts from a CSV file. */
	QAction *importAct = menu_->addAction("Import CSV ...");
	connect(importAct, &QAction::triggered, this, &TrayApp::onImportCsv);

	QAction *aboutAct = menu_->addAction("About ...");
	connect(aboutAct, &QAction::triggered, this, &TrayApp::onAbout);

	menu_->addSeparator();

	QAction *quitAct = menu_->addAction("Quit");
	connect(quitAct, &QAction::triggered, this, &TrayApp::onQuit);
}


void TrayApp::populateAccounts()
{
	struct le *le;

	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = static_cast<struct ua *>(le->data);

		/* Label is "<display name>  sip:<user>" — the @domain
		 * part of the AOR is suppressed. */
		QString label = accountLabel(ua);
		if (ua_isregistered(ua))
			label += " (OK)";

		QAction *act = accountsMenu_->addAction(label);
		act->setCheckable(true);
		act->setData(QVariant::fromValue<quintptr>(
					reinterpret_cast<quintptr>(ua)));
		accountsGroup_->addAction(act);

		if (ua == qt_current_ua())
			act->setChecked(true);
	}

	if (!accountsGroup_->checkedAction()) {
		QList<QAction *> actions = accountsGroup_->actions();
		if (!actions.isEmpty())
			actions.first()->setChecked(true);
	}
}


QAction *TrayApp::findAccountAction(quintptr uaPtr) const
{
	for (QAction *act : accountsGroup_->actions()) {
		if (act->data().value<quintptr>() == uaPtr)
			return act;
	}
	return nullptr;
}


void TrayApp::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
	/* Remember where the tray icon lives: under Wayland the icon
	 * geometry isn't exposed, but the cursor is over the icon when
	 * it is activated, so its position is a good anchor proxy. */
	if (reason == QSystemTrayIcon::Trigger ||
	    reason == QSystemTrayIcon::Context ||
	    reason == QSystemTrayIcon::MiddleClick)
		trayClickPos_ = QCursor::pos();

	/* Reset the icon back to normal once the user has seen it
	 * (e.g. after a missed-call icon was shown).
	 */
	if (reason == QSystemTrayIcon::Trigger ||
	    reason == QSystemTrayIcon::Context) {
		setTrayIcon("call-start", QString());
	}

	/* NB: do NOT call menu_->popup() here. Under KDE Plasma the
	 * right-click context menu isn't shown via Qt's own popup at
	 * all -- it's exported over D-Bus (StatusNotifierItem/DBusMenu)
	 * and Plasma renders it as its own Wayland surface. A real Qt
	 * popup grab (menu_->popup()) requires a focused parent window,
	 * which the tray icon isn't, and fails under Wayland ("Failed
	 * to create grabbing popup"). Left-click has no menu equivalent
	 * in this protocol (it's reserved for a separate "Activate"
	 * action) -- give it a useful default instead of nothing.
	 */
	if (reason == QSystemTrayIcon::Trigger) {
		/* 1. If the idle dialer is open, toggle it closed.
		 * 2. If any other panel (settings or an active call
		 *    dialog) is open, close it — the next click can
		 *    then open the dialer.
		 * 3. Otherwise open the dialer. */
		if (idleCallDialog_ && idleCallDialog_->isVisible()) {
			idleCallDialog_->hide();
		} else if ((settingsDialog_ &&
			   settingsDialog_->isVisible()) ||
			  !callDialogs_.isEmpty()) {
			if (settingsDialog_ && settingsDialog_->isVisible())
				settingsDialog_->close();
			/* Close any active call dialogs. */
			for (auto it = callDialogs_.begin();
			     it != callDialogs_.end(); ) {
				QPointer<CallDialog> dlg = it.value();
				if (dlg && dlg != idleCallDialog_)
					dlg->close();
				it = callDialogs_.erase(it);
			}
		} else {
			onDial();
		}
	}
}


void TrayApp::onDial()
{
	/* Reuse a single idle Dialing dialog so repeated clicks don't
	 * pile up multiple windows. Non-modal (show, not exec) avoids
	 * Qt's "Recursive call detected" warning when an incoming call
	 * arrives while a dialog is open. */
	if (!idleCallDialog_)
		idleCallDialog_ = new CallDialog(trayIcon_);

	/* Wire up the green button -- place the call when clicked. */
	disconnect(idleCallDialog_, nullptr, this, nullptr);
	connect(idleCallDialog_, &CallDialog::callRequested,
		this, [this](QString uri) {
		QByteArray u = uri.toUtf8();
		qt_mod_connect(u.constData());
	});

	idleCallDialog_->setAnchorPoint(trayClickPos_);
	idleCallDialog_->showPanel();
}


void TrayApp::onAbout()
{
	QMessageBox::about(nullptr, "About baresip",
		QString("<b>baresip</b> %1<br>"
			"A modular SIP User-Agent with audio "
			"and video support<br><br>"
			"Copyright (C) 2010 - 2025 "
			"Alfred E. Heggestad et al.<br>"
			"License: BSD<br>"
			"<a href=\"https://github.com/baresip/baresip\">"
			"https://github.com/baresip/baresip</a>")
			.arg(QString::fromUtf8(baresip_version())));
}


void TrayApp::onQuit()
{
	qt_mod_quit();
}


void TrayApp::onSettings()
{
	if (!settingsDialog_)
		settingsDialog_ = new SettingsDialog();

	settingsDialog_->setAnchorPoint(trayClickPos_);
	settingsDialog_->show();
	settingsDialog_->raise();
	settingsDialog_->activateWindow();
}


void TrayApp::onAccountToggled(QAction *action)
{
	if (!action->isChecked())
		return;

	quintptr uaPtr = action->data().value<quintptr>();
	qt_mod_select_ua(reinterpret_cast<struct ua *>(uaPtr));
}


void TrayApp::onPresenceToggled()
{
	presenceOpen_ = !presenceOpen_;

	enum presence_status status =
		presenceOpen_ ? PRESENCE_OPEN : PRESENCE_CLOSED;

	struct le *le;
	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = static_cast<struct ua *>(le->data);
		ua_presence_status_set(ua, status);
	}

	/* Persist the choice so it survives restarts. */
	QFile f(presencePath());
	if (f.open(QIODevice::WriteOnly | QIODevice::Text))
		f.write(presenceOpen_ ? "open\n" : "closed\n");

	presenceAct_->setIcon(spotIcon(presenceOpen_
		? QColor("#4caf50") : QColor("#e53935")));
}


void TrayApp::publishPresence(quintptr uaPtr)
{
	/* A UA (re)registered — apply the toggle state to it so the
	 * presence module publishes the user's chosen status. */
	struct ua *ua = reinterpret_cast<struct ua *>(uaPtr);
	ua_presence_status_set(ua,
		presenceOpen_ ? PRESENCE_OPEN : PRESENCE_CLOSED);
}


void TrayApp::openDialNumber(QString number)
{
	/* tel: link / qtdial command: open the dial panel with the
	 * number populated — the user confirms with the green button
	 * (which also becomes the hangup once the call starts). */
	onDial();
	if (idleCallDialog_ && !number.isEmpty())
		idleCallDialog_->setDialNumber(number);
}


void TrayApp::onImportCsv()
{
	/* Open the dial panel and switch to the contacts view, then
	 * show the import instructions overlay — the file browser is
	 * opened from the overlay's Import button. */
	onDial();
	if (idleCallDialog_) {
		idleCallDialog_->showContacts(true);
		idleCallDialog_->showImportInstructions();
	}
}


void TrayApp::onAnswer(quintptr callPtr)
{
	qt_mod_answer(reinterpret_cast<struct call *>(callPtr));

	/* Swap Accept/Reject for Hang Up immediately; don't wait for the
	 * BEVENT_CALL_ESTABLISHED round trip, since answering commits us
	 * to the call either way and the user needs a way to end it now.
	 */
	QMenu *callMenu = callMenus_.value(callPtr, nullptr);
	if (callMenu)
		convertToHangup(callPtr, callMenu->title());
}


void TrayApp::onReject(quintptr callPtr, QString peerUri, QString peerName)
{
	/* History for rejected/missed calls is now recorded at the
	 * event level (BEVENT_CALL_CLOSED). */
	qt_mod_hangup(reinterpret_cast<struct call *>(callPtr));
}


void TrayApp::onHangup(quintptr callPtr)
{
	qt_mod_hangup(reinterpret_cast<struct call *>(callPtr));
}


/* ---- slots invoked (via queued connection) from the re/core thread ---- */

void TrayApp::accountsChanged()
{
	accountsMenu_->clear();
	for (QAction *a : accountsGroup_->actions())
		accountsGroup_->removeAction(a);
	populateAccounts();
	refreshTrayMenu();
}


void TrayApp::accountStatus(quintptr uaPtr, QString label, QString status)
{
	QAction *act = findAccountAction(uaPtr);
	if (!act) {
		/* Account added after startup; just rebuild. */
		accountsChanged();
		return;
	}

	act->setText(QString("%1 (%2)").arg(label, status));
}


QMenu *TrayApp::addCallMenu(quintptr callPtr, const QString &title)
{
	/* NB: return the menu unattached. The caller must fully populate
	 * it with actions *before* inserting it into menu_ (done at the
	 * end of callIncoming()/callOutgoing() below) -- attaching an
	 * empty submenu and filling it in afterward can lose the fill
	 * under Plasma's DBusMenu export, since that export effectively
	 * snapshots the structure at attach time.
	 */
	(void)callPtr;
	return new QMenu(title);
}


void TrayApp::convertToHangup(quintptr callPtr, const QString &peerUri)
{
	QMenu *callMenu = callMenus_.value(callPtr, nullptr);
	if (!callMenu)
		return;

	callMenu->setTitle(QString("Call: %1").arg(peerUri));
	callMenu->clear();

	QAction *info = callMenu->addAction(peerUri);
	info->setEnabled(false);

	QAction *hangupAct = callMenu->addAction("Hang Up");
	connect(hangupAct, &QAction::triggered, this, [this, callPtr]() {
		onHangup(callPtr);
	});

	refreshTrayMenu();
}


void TrayApp::callIncoming(quintptr callPtr, QString peerUri,
			    QString peerName)
{
	/* Close any open dial/settings panels so the call-control
	 * panel has the user's attention. */
	if (idleCallDialog_ && idleCallDialog_->isVisible())
		idleCallDialog_->hide();
	if (settingsDialog_ && settingsDialog_->isVisible())
		settingsDialog_->close();

	setTrayIcon("call-incoming-symbolic", "go-next");

	QMenu *callMenu = addCallMenu(callPtr,
			QString("Incoming call: %1").arg(peerName));

	QAction *info = callMenu->addAction(peerUri);
	info->setEnabled(false);

	QAction *acceptAct = callMenu->addAction("Accept");
	connect(acceptAct, &QAction::triggered, this, [this, callPtr]() {
		onAnswer(callPtr);
	});

	QAction *rejectAct = callMenu->addAction("Reject");
	connect(rejectAct, &QAction::triggered,
		this, [this, callPtr, peerUri, peerName]() {
			onReject(callPtr, peerUri, peerName);
		});

	/* Fully populated now -- attach it. */
	menu_->insertMenu(menu_->actions().first(), callMenu);
	callMenus_.insert(callPtr, callMenu);
	refreshTrayMenu();

	/* Pop up a non-modal call-control dialog with green=Answer,
	 * red=Hangup. If the idle Dialing dialog is currently visible,
	 * repurpose it instead of opening a second window. */
	CallDialog *dlg = nullptr;
	if (idleCallDialog_ && idleCallDialog_->isVisible()) {
		dlg = idleCallDialog_;
		/* Disconnect the Dialing-state callRequested signal;
		 * the dialog will now emit answerRequested/hangupRequested. */
		disconnect(dlg, &CallDialog::callRequested, this, nullptr);
		dlg->setStateIncoming(callPtr, peerUri, peerName);
	} else {
		dlg = new CallDialog(CallDialog::State::Incoming, callPtr,
				     peerUri, peerName, trayIcon_);
	}
	callDialogs_.insert(callPtr, dlg);
	dlg->setAnchorPoint(trayClickPos_);

	connect(dlg, &CallDialog::answerRequested,
		this, [this, callPtr]() { onAnswer(callPtr); });
	connect(dlg, &CallDialog::hangupRequested,
		this, [this, callPtr]() { onHangup(callPtr); });

	if (dlg != idleCallDialog_) {
		dlg->showPanel();
	} else {
		dlg->raise();
		dlg->activateWindow();
	}

	trayIcon_->showMessage("Incoming call",
				QString("%1 <%2>").arg(peerName, peerUri),
				QSystemTrayIcon::Information, 10000);
}


void TrayApp::callOutgoing(quintptr callPtr, QString peerUri)
{
	QMenu *callMenu = addCallMenu(callPtr,
			QString("Calling: %1").arg(peerUri));

	QAction *info = callMenu->addAction(peerUri);
	info->setEnabled(false);

	QAction *hangupAct = callMenu->addAction("Hang Up");
	connect(hangupAct, &QAction::triggered, this, [this, callPtr]() {
		onHangup(callPtr);
	});

	/* Fully populated now -- attach it. */
	menu_->insertMenu(menu_->actions().first(), callMenu);
	callMenus_.insert(callPtr, callMenu);
	refreshTrayMenu();

	/* Pop up a non-modal call-control panel in InCall (ringing-out)
	 * state: green=Call (disabled), red=Hangup, plus Dialpad. */
	auto *dlg = new CallDialog(CallDialog::State::InCall, callPtr,
				   peerUri, QString(), trayIcon_);
	callDialogs_.insert(callPtr, dlg);
	dlg->setAnchorPoint(trayClickPos_);

	connect(dlg, &CallDialog::hangupRequested,
		this, [this, callPtr]() { onHangup(callPtr); });

	dlg->showPanel();
}


void TrayApp::callClosed(quintptr callPtr, bool missed,
			  QString peerUri, QString peerName)
{
	QMenu *callMenu = callMenus_.take(callPtr);
	if (callMenu) {
		menu_->removeAction(callMenu->menuAction());
		callMenu->deleteLater();
		refreshTrayMenu();
	}

	QPointer<CallDialog> cdlg = callDialogs_.take(callPtr);
	if (cdlg) {
		/* If this was the repurposed idle dialog, reset it to the
		 * Dialing state so it can be reused; otherwise close it. */
		if (cdlg == idleCallDialog_) {
			disconnect(cdlg, &CallDialog::answerRequested, this, nullptr);
			disconnect(cdlg, &CallDialog::hangupRequested, this, nullptr);
			cdlg->setStateDialing();
			/* Re-wire the green button for outgoing calls. */
			connect(cdlg, &CallDialog::callRequested,
				this, [this](QString uri) {
				QByteArray u = uri.toUtf8();
				qt_mod_connect(u.constData());
			});
		} else {
			cdlg->close();
		}
	}

	if (missed) {
		setTrayIcon("call-missed-symbolic", "call-stop");
	}
}


void TrayApp::callEstablished(quintptr callPtr)
{
	QMenu *callMenu = callMenus_.value(callPtr, nullptr);
	if (callMenu) {
		/* peer URI is whatever's already shown on the disabled
		 * info action (first action in the menu)
		 */
		QList<QAction *> acts = callMenu->actions();
		QString peerUri = acts.isEmpty() ? QString() : acts.first()->text();
		convertToHangup(callPtr, peerUri);
	}

	/* Transition the call-control dialog to InCall (Connected). */
	QPointer<CallDialog> cdlg = callDialogs_.value(callPtr, nullptr);
	if (cdlg) {
		QList<QAction *> acts = callMenu ? callMenu->actions() : QList<QAction*>();
		QString peerUri = acts.isEmpty() ? QString() : acts.first()->text();
		cdlg->setStateInCall(peerUri);
	}

	setTrayIcon("call-start", QString());
}


void TrayApp::addHistory(QString number, QString uri, int callType,
			 QString info)
{
	/* Persist to ~/.baresip/call_history.csv and refresh the idle
	 * CallDialog's history list if it's open. */
	CallHistory::instance()->add(number, uri, callType, info);
	if (idleCallDialog_)
		idleCallDialog_->refreshHistory();
}


void TrayApp::updateHistoryDuration(QString uri, uint duration,
				    int callType)
{
	CallHistory::instance()->updateDuration(uri, duration, callType);
	if (idleCallDialog_)
		idleCallDialog_->refreshHistory();
}


void TrayApp::showWarning(QString title, QString text)
{
	/* NB: baresip.h #defines "warning" as a logging macro, which
	 * collides with the QMessageBox::warning(...) static method name.
	 * Build the box manually instead of calling that method.
	 */
	QMessageBox box(QMessageBox::Warning, title, text,
			 QMessageBox::Ok, nullptr);
	box.exec();
}
