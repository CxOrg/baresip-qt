/**
 * @file qt/tray_app.cpp Qt UI module -- tray icon + menu
 */
#include "tray_app.h"
#include "call_dialog.h"
#include "call_history.h"
#include "settings_dialog.h"
#include "dialpad_dialog.h"

#include <QMessageBox>
#include <QInputDialog>
#include <QIcon>
#include <QDateTime>
#include <QApplication>
#include <QCursor>


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


void TrayApp::buildMenu()
{
	menu_ = new QMenu();

	/* Account submenu */
	accountsMenu_ = menu_->addMenu("Account");
	accountsGroup_ = new QActionGroup(this);
	accountsGroup_->setExclusive(true);
	connect(accountsGroup_, &QActionGroup::triggered,
		this, &TrayApp::onAccountToggled);
	populateAccounts();

	/* Status submenu */
	statusMenu_ = menu_->addMenu("Status");
	statusGroup_ = new QActionGroup(this);
	statusGroup_->setExclusive(true);
	connect(statusGroup_, &QActionGroup::triggered,
		this, &TrayApp::onStatusToggled);

	QAction *openAct = statusMenu_->addAction("Open");
	openAct->setCheckable(true);
	openAct->setChecked(true);
	openAct->setData(static_cast<int>(PRESENCE_OPEN));
	statusGroup_->addAction(openAct);

	QAction *closedAct = statusMenu_->addAction("Closed");
	closedAct->setCheckable(true);
	closedAct->setData(static_cast<int>(PRESENCE_CLOSED));
	statusGroup_->addAction(closedAct);

	menu_->addSeparator();

	/* Dial */
	QAction *dialAct = menu_->addAction("Dial...");
	connect(dialAct, &QAction::triggered, this, &TrayApp::onDial);

	/* Dial contact */
	contactsMenu_ = menu_->addMenu("Dial contact");
	populateContacts();

	/* Call history */
	historyMenu_ = menu_->addMenu("Call history");
	populateHistoryMenu();

	menu_->addSeparator();

	QAction *aboutAct = menu_->addAction("About");
	connect(aboutAct, &QAction::triggered, this, &TrayApp::onAbout);

	QAction *settingsAct = menu_->addAction("Settings...");
	connect(settingsAct, &QAction::triggered, this, &TrayApp::onSettings);

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


void TrayApp::populateContacts()
{
	struct contacts *contacts = baresip_contacts();
	struct le *le;

	for (le = list_head(contact_list(contacts)); le; le = le->next) {
		struct contact *c = static_cast<struct contact *>(le->data);

		/* Label is "Name  number" — the SIP URI is stripped.
		 * The bare number is stored in the action and goes to
		 * the dial field; the full URI is reconstructed on
		 * dialing (account_uri_complete_strdup). */
		QString number = uriToNumber(contact_uri(c));
		QString name;
		const struct sip_addr *addr = contact_addr(c);
		if (addr && pl_isset(&addr->dname))
			name = QString::fromUtf8(addr->dname.p,
						 (int)addr->dname.l)
				.remove('"').trimmed();

		QString label = name.isEmpty()
			? number
			: QString("%1  %2").arg(name, number);

		QAction *act = contactsMenu_->addAction(label);
		act->setData(number);
		connect(act, &QAction::triggered, this, [this, act]() {
			onDialContact(act);
		});
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
		/* Toggle: if the idle dialog is visible, hide it;
		 * otherwise show it. */
		if (idleCallDialog_ && idleCallDialog_->isVisible()) {
			idleCallDialog_->hide();
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


void TrayApp::onStatusToggled(QAction *action)
{
	if (!action->isChecked())
		return;

	enum presence_status status =
		static_cast<enum presence_status>(action->data().toInt());

	struct le *le;
	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = static_cast<struct ua *>(le->data);
		ua_presence_status_set(ua, status);
	}
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


void TrayApp::onDialContact(QAction *action)
{
	/* Pass the contact's number to the dial panel — the user
	 * presses the green button to place the call. */
	QString number = action->data().toString();
	if (number.isEmpty())
		return;

	onDial();
	idleCallDialog_->setDialNumber(number);
}


void TrayApp::onDialHistory(QAction *action)
{
	/* URI is stored in the action's data (see addHistory). */
	QByteArray uri = action->data().toString().toUtf8();
	if (!uri.isEmpty())
		qt_mod_connect(uri.constData());
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


void TrayApp::openDialpad(quintptr callPtr, QString peerLabel)
{
	QPointer<DialpadDialog> &existing = dialpads_[callPtr];
	if (existing) {
		existing->raise();
		existing->activateWindow();
		return;
	}

	auto *dlg = new DialpadDialog(callPtr, peerLabel);
	dialpads_[callPtr] = dlg;
	dlg->show();
}


void TrayApp::addDialpadAction(QMenu *callMenu, quintptr callPtr,
				const QString &peerLabel)
{
	QAction *dialpadAct = callMenu->addAction("Dialpad...");
	connect(dialpadAct, &QAction::triggered,
		this, [this, callPtr, peerLabel]() {
			openDialpad(callPtr, peerLabel);
		});
}


/* ---- slots invoked (via queued connection) from the re/core thread ---- */

void TrayApp::accountStatus(quintptr uaPtr, QString label, QString status)
{
	QAction *act = findAccountAction(uaPtr);
	if (!act) {
		/* Account added after startup; just rebuild. */
		accountsMenu_->clear();
		for (QAction *a : accountsGroup_->actions())
			accountsGroup_->removeAction(a);
		populateAccounts();
		refreshTrayMenu();
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

	addDialpadAction(callMenu, callPtr, peerUri);

	refreshTrayMenu();
}


void TrayApp::callIncoming(quintptr callPtr, QString peerUri,
			    QString peerName)
{
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

	connect(dlg, &CallDialog::answerRequested,
		this, [this, callPtr]() { onAnswer(callPtr); });
	connect(dlg, &CallDialog::hangupRequested,
		this, [this, callPtr]() { onHangup(callPtr); });
	connect(dlg, &CallDialog::dialpadRequested,
		this, [this](quintptr cp, QString label) {
			openDialpad(cp, label);
		});

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

	addDialpadAction(callMenu, callPtr, peerUri);

	/* Fully populated now -- attach it. */
	menu_->insertMenu(menu_->actions().first(), callMenu);
	callMenus_.insert(callPtr, callMenu);
	refreshTrayMenu();

	/* Pop up a non-modal call-control panel in InCall (ringing-out)
	 * state: green=Call (disabled), red=Hangup, plus Dialpad. */
	auto *dlg = new CallDialog(CallDialog::State::InCall, callPtr,
				   peerUri, QString(), trayIcon_);
	callDialogs_.insert(callPtr, dlg);

	connect(dlg, &CallDialog::hangupRequested,
		this, [this, callPtr]() { onHangup(callPtr); });
	connect(dlg, &CallDialog::dialpadRequested,
		this, [this](quintptr cp, QString label) {
			openDialpad(cp, label);
		});

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

	QPointer<DialpadDialog> dlg = dialpads_.take(callPtr);
	if (dlg)
		dlg->close();

	QPointer<CallDialog> cdlg = callDialogs_.take(callPtr);
	if (cdlg) {
		/* If this was the repurposed idle dialog, reset it to the
		 * Dialing state so it can be reused; otherwise close it. */
		if (cdlg == idleCallDialog_) {
			disconnect(cdlg, &CallDialog::answerRequested, this, nullptr);
			disconnect(cdlg, &CallDialog::hangupRequested, this, nullptr);
			disconnect(cdlg, &CallDialog::dialpadRequested, this, nullptr);
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


QAction *TrayApp::makeHistoryAction(const QString &uri, int callType,
				    const QString &info, const QDateTime &ts,
				    uint32_t duration)
{
	QString iconName, fallback;

	switch (callType) {
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

	/* Single-line label matching the CallDialog history list:
	 * "uri  (M:SS)  MM-dd hh:mm" — duration only when connected. */
	QString label;
	if (duration > 0) {
		QString dur = QString("%1:%2")
			.arg(duration / 60)
			.arg(duration % 60, 2, 10, QChar('0'));
		label = QString("%1  (%3)  %2")
			.arg(info.isEmpty() ? uri : info,
			     ts.toString("MM-dd hh:mm"), dur);
	}
	else {
		label = QString("%1  %2")
			.arg(info.isEmpty() ? uri : info,
			     ts.toString("MM-dd hh:mm"));
	}

	QAction *act = new QAction(label);
	QIcon icon = QIcon::fromTheme(iconName);
	if (icon.isNull() && !fallback.isEmpty())
		icon = QIcon::fromTheme(fallback);
	if (!icon.isNull())
		act->setIcon(icon);
	act->setData(uri);
	connect(act, &QAction::triggered, this, [this, act]() {
		onDialHistory(act);
	});

	return act;
}


void TrayApp::populateHistoryMenu()
{
	/* Load persisted entries (oldest first) so the submenu shows
	 * history across restarts, not just calls from this session. */
	auto entries = CallHistory::instance()->recent(20);
	for (const CallHistoryEntry &e : entries)
		historyMenu_->addAction(makeHistoryAction(
			e.uri, e.type, e.info, e.ts, e.duration));

	historyLength_ = entries.size();
}


void TrayApp::addHistory(QString uri, int callType, QString info)
{
	/* Persist to ~/.baresip/call_history.csv and refresh the idle
	 * CallDialog's history list if it's open. */
	CallHistory::instance()->add(uri, callType, info);
	if (idleCallDialog_)
		idleCallDialog_->refreshHistory();

	QAction *act = makeHistoryAction(uri, callType, info,
					 QDateTime::currentDateTime(), 0);

	if (historyLength_ >= 20) {
		QList<QAction *> acts = historyMenu_->actions();
		if (!acts.isEmpty()) {
			QAction *oldest = acts.first();
			historyMenu_->removeAction(oldest);
			oldest->deleteLater();
		}
	}
	else {
		historyLength_++;
	}

	historyMenu_->addAction(act);
}


void TrayApp::updateHistoryDuration(QString uri, uint duration)
{
	CallHistory::instance()->updateDuration(uri, duration);
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
