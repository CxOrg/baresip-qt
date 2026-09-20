/**
 * @file qt/call_dialog.h Qt UI module -- unified call control panel
 *
 * A call control panel embedded in a QMenu (via QWidgetAction) that
 * appears near the tray icon on left-click, adapting to three states:
 *
 *  - Dialing:   number entry editable; green=Call, red=Cancel
 *  - Incoming:  number read-only;      green=Answer, red=Hangup
 *  - InCall:    number read-only;     green=Call/Answer (disabled),
 *               red=Hangup, optional Dialpad
 *
 * Using a QMenu container gives us automatic tray-anchored positioning,
 * native Plasma styling, and Wayland popup support -- no LayerShellQt
 * or manual window positioning needed.
 */
#pragma once

#include <QWidget>
#include <QString>
#include <QPoint>

class QLineEdit;
class QPushButton;
class QListWidget;
class QListWidgetItem;
class QMenu;

class CallDialog : public QWidget {
	Q_OBJECT

public:
	enum class State {
		Dialing,   /**< Idle -- user entering a number to dial */
		Incoming,  /**< Remote is calling us */
		InCall,    /**< Call is up (or ringing out) */
	};

	/** Dialing state: editable entry, green=Call, red=Cancel.
	 *  If `prefill` is non-empty the entry is populated but the call
	 *  is NOT placed until the green button is clicked.
	 *  `anchor` is the screen coordinate of the tray icon (from
	 *  StatusNotifierItem::Activate) used for panel positioning. */
	explicit CallDialog(QPoint anchor = QPoint(),
			    QWidget *parent = nullptr);

	/** Refresh the history list from the persistent store. Only
	 *  shown in Dialing state. */
	void refreshHistory();

	/** Incoming/InCall state: read-only entry showing the peer URI. */
	CallDialog(State state, quintptr callPtr, const QString &peerUri,
		   const QString &peerName, QPoint anchor = QPoint(),
		   QWidget *parent = nullptr);

	/** Switch an existing dialog to InCall (used after Accept or
	 *  after an outgoing call connects). */
	void setStateInCall(const QString &peerUri);

	/** Reset an existing dialog back to Dialing (used when a
	 *  repurposed idle dialog's call ends). */
	void setStateDialing();

	/** Switch an existing dialog to Incoming (used if a Dialing
	 *  dialog is repurposed for an incoming call). */
	void setStateIncoming(quintptr callPtr, const QString &peerUri,
			      const QString &peerName);

	/** Show the panel as a QMenu popup positioned near the tray
	 *  icon. The QMenu provides native Wayland popup behavior and
	 *  Plasma styling. */
	void showPanel();

	/** Close the popup menu if one is open. */
	void closePanel();

	/** Raise/activate -- for compatibility with TrayApp. */
	void raise() {}
	void activateWindow() {}

	quintptr callPtr() const { return callPtr_; }
	State state() const { return state_; }
	bool isVisible() const;

signals:
	/** Green button clicked. In Dialing: emit the URI to dial.
	 *  In Incoming: emit the callPtr to answer. */
	void callRequested(QString uri);
	void answerRequested(quintptr callPtr);

	/** Red button clicked. In Dialing: just close. In Incoming:
	 *  reject the call. In InCall: hang up. */
	void hangupRequested(quintptr callPtr);
	void rejectRequested(quintptr callPtr);

	/** User opened the in-call DTMF dialpad. */
	void dialpadRequested(quintptr callPtr, QString peerLabel);

private:
	void buildUi();
	void applyState();
	void onMenuAboutToHide();

	State state_;
	quintptr callPtr_ = 0;
	QString peerName_;
	bool isOutgoing_ = false;  /**< direction for InCall label */
	QPoint anchor_;            /**< tray icon screen position for positioning */

	QMenu *menu_ = nullptr;    /**< popup menu containing this widget */

	QLineEdit    *uriEdit_    = nullptr;
	QPushButton  *greenBtn_   = nullptr;
	QPushButton  *redBtn_     = nullptr;
	QPushButton  *dialpadBtn_ = nullptr;
	QListWidget  *historyList_ = nullptr;

private slots:
	void onGreen();
	void onRed();
	void onDialpad();
	void onHistoryClicked(QListWidgetItem *item);
	void onHistoryDoubleClicked(QListWidgetItem *item);
};
