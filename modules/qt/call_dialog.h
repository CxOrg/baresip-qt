/**
 * @file qt/call_dialog.h Qt UI module -- unified call control dialog
 *
 * A single non-modal dialog that adapts to three call states:
 *
 *  - Dialing:   number entry editable; green=Call, red=Cancel
 *  - Incoming:  number read-only;      green=Answer, red=Hangup
 *  - InCall:    number read-only;     green=Call/Answer (disabled),
 *               red=Hangup, optional Dialpad
 *
 * The dialog is non-modal (show(), not exec()) so it can coexist
 * with tray menus and multiple simultaneous calls without triggering
 * Qt's "Recursive call detected" warning.
 */
#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;
class QListWidget;
class QListWidgetItem;

class CallDialog : public QDialog {
	Q_OBJECT

public:
	enum class State {
		Dialing,   /**< Idle -- user entering a number to dial */
		Incoming,  /**< Remote is calling us */
		InCall,    /**< Call is up (or ringing out) */
	};

	/** Dialing state: editable entry, green=Call, red=Cancel.
	 *  If `prefill` is non-empty the entry is populated but the call
	 *  is NOT placed until the green button is clicked. */
	explicit CallDialog(QWidget *parent = nullptr);

	/** Refresh the history list from the persistent store. Only
	 *  shown in Dialing state. */
	void refreshHistory();

	/** Incoming/InCall state: read-only entry showing the peer URI. */
	CallDialog(State state, quintptr callPtr, const QString &peerUri,
		   const QString &peerName, QWidget *parent = nullptr);

	/** Switch an existing dialog to InCall (used after Accept or
	 *  after an outgoing call connects). */
	void setStateInCall(const QString &peerUri);

	/** Switch an existing dialog to Incoming (used if a Dialing
	 *  dialog is repurposed for an incoming call). */
	void setStateIncoming(quintptr callPtr, const QString &peerUri,
			      const QString &peerName);

	quintptr callPtr() const { return callPtr_; }
	State state() const { return state_; }

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

	State state_;
	quintptr callPtr_ = 0;
	QString peerName_;
	bool isOutgoing_ = false;  /**< direction for InCall label */

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
