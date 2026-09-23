/**
 * @file qt/tray_app.h Qt UI module -- tray icon + menu
 */
#pragma once

#include "qt_mod.h"

#include <QSystemTrayIcon>
#include <QMenu>
#include <QActionGroup>
#include <QHash>
#include <QPointer>
#include <QPoint>
#include <QDateTime>

class CallDialog;
class SettingsDialog;

class TrayApp : public QObject {
	Q_OBJECT

public:
	explicit TrayApp(struct qt_mod *mod, QObject *parent = nullptr);
	~TrayApp() override;

	void show();

public slots:
	/* Called (via queued connection) from baresip's event handler,
	 * which runs on the re/core thread.
	 */
	void accountStatus(quintptr uaPtr, QString label, QString status);
	/* Rebuilds the Online Accounts menu after a UA is created or
	 * destroyed (settings apply, disable/enable). */
	void accountsChanged();
	/* Re-applies the presence toggle state to a UA that has just
	 * registered, so the status is published on connect. */
	void publishPresence(quintptr uaPtr);
	void callIncoming(quintptr callPtr, QString peerUri, QString peerName);
	void callOutgoing(quintptr callPtr, QString peerUri);
	void callClosed(quintptr callPtr, bool missed,
			 QString peerUri, QString peerName);
	void callEstablished(quintptr callPtr);
	void addHistory(QString number, QString uri, int callType,
			QString info);
	void updateHistoryDuration(QString uri, uint duration);
	void showWarning(QString title, QString text);
	/* Opens the dial panel with a number pre-filled (tel: links via
	 * the "qtdial" command). Runs on the Qt thread. */
	void openDialNumber(QString number);

private slots:
	void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
	void onDial();
	void onAbout();
	void onSettings();
	void onImportCsv();
	void onQuit();
	void onAccountToggled(QAction *action);
	void onPresenceToggled();
	void onAnswer(quintptr callPtr);
	void onReject(quintptr callPtr, QString peerUri, QString peerName);
	void onHangup(quintptr callPtr);

private:
	void buildMenu();
	void populateAccounts();
	QAction *findAccountAction(quintptr uaPtr) const;
	void setTrayIcon(const QString &themeName, const QString &fallback);
	QMenu *addCallMenu(quintptr callPtr, const QString &title);
	void convertToHangup(quintptr callPtr, const QString &peerUri);
	void refreshTrayMenu();

	struct qt_mod *mod_;

	QSystemTrayIcon *trayIcon_ = nullptr;
	QMenu *menu_ = nullptr;
	QMenu *accountsMenu_ = nullptr;
	QAction *presenceAct_ = nullptr;
	QActionGroup *accountsGroup_ = nullptr;
	bool presenceOpen_ = true;

	/* Unified call-control dialog. One per active call (keyed by
	 * call pointer), plus a singleton for the idle "Dial" state. */
	CallDialog *idleCallDialog_ = nullptr;
	QHash<quintptr, QPointer<CallDialog>> callDialogs_;

	SettingsDialog *settingsDialog_ = nullptr;

	/* Per-call submenus, keyed by call pointer. Created the moment a
	 * call starts (incoming ring, or outgoing dial) and kept until
	 * BEVENT_CALL_CLOSED -- covers the full lifecycle so there is
	 * always a Hang Up action available for any live call.
	 */
	QHash<quintptr, QMenu *> callMenus_;

	/* Cursor position at the last tray-icon activation -- a proxy
	 * for the icon's screen position (the cursor is over the icon
	 * when clicked) used to anchor popup panels under Wayland. */
	QPoint trayClickPos_;
};
