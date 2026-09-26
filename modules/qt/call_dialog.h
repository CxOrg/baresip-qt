/**
 * @file qt/call_dialog.h Qt UI module -- unified call control panel
 *
 * A frameless popup panel that appears near the tray icon on
 * left-click, adapting to three call states:
 *
 *  - Dialing:   number entry editable; green=Call, red=Cancel
 *  - Incoming:  number read-only;      green=Answer, red=Hangup
 *  - InCall:    number read-only;     green=Call/Answer (disabled),
 *               red=Hangup, optional Dialpad
 *
 * The panel uses Qt::Popup | Qt::FramelessWindowHint so it appears
 * as a lightweight popup extending from the tray icon (similar to
 * a menu) and closes when the user clicks outside it. It inherits
 * the Plasma/system Qt style automatically.
 */
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QList>
#include <functional>

class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QSystemTrayIcon;
class QFrame;
class QIcon;
class QResizeEvent;
class QHideEvent;
class QTabBar;
class QComboBox;

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
	 *  is NOT placed until the green button is clicked.
	 *  If `trayIcon` is set the panel positions itself near it. */
	explicit CallDialog(QSystemTrayIcon *trayIcon = nullptr,
			    QWidget *parent = nullptr);

	/** Refresh the history list from the persistent store. Only
	 *  shown in Dialing state. */
	void refreshHistory();

	/** Switch the list between the history view (false) and the
	 *  contacts view (true); refreshes the list and the toggle
	 *  button text. */
	void showContacts(bool contacts);

	/** Import contacts from a CSV file. Parses name/phone columns,
	 *  builds records, shows a confirmation overlay, and on Yes
	 *  merges with existing contacts, sorts, and saves. */
	void importCsv(const QString &path);

	/** Show the CSV import instructions overlay with Import and
	 *  Cancel buttons. Import opens the file browser; Cancel
	 *  closes the overlay. */
	void showImportInstructions();

	/** Populate the dial entry with a number (Dialing state).
	 *  The call is NOT placed until the green button is clicked. */
	void setDialNumber(const QString &number);

	/** Incoming/InCall state: read-only entry showing the peer URI. */
	CallDialog(State state, quintptr callPtr, const QString &peerUri,
		   const QString &peerName, QSystemTrayIcon *trayIcon = nullptr,
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

	/** Show the panel positioned near the tray icon. */
	void showPanel();

	/** Screen position of the tray icon (e.g. cursor position at
	 *  activation time) used to anchor the panel; null = default
	 *  top-right placement. */
	void setAnchorPoint(const QPoint &pos) { anchorPos_ = pos; }

	/** Close the panel if the user clicked outside it (popup-like
	 *  behavior without Qt::Popup, which doesn't work on Wayland
	 *  without a transient parent). */
	bool eventFilter(QObject *obj, QEvent *event) override;

	quintptr callPtr() const { return callPtr_; }
	State state() const { return state_; }

	/** One parsed ~/.baresip/contacts line. Public so the
	 *  file-local parse/format helpers in the .cpp can use it. */
	struct ContactEntry {
		QString name;
		QString type;   /**< contact type (Work, Home, ...). */
		QString uri;
		QString params;
	};

signals:
	/** Green button clicked. In Dialing: emit the URI to dial.
	 *  In Incoming: emit the callPtr to answer. */
	void callRequested(QString uri);
	void answerRequested(quintptr callPtr);

	/** Red button clicked. In Dialing: just close. In Incoming:
	 *  reject the call. In InCall: hang up. */
	void hangupRequested(quintptr callPtr);

	/** Emitted after the contacts file was rewritten — the tray
	 *  repopulates the Call Contact submenu. */
	void contactsSaved();

protected:
	void resizeEvent(QResizeEvent *ev) override;
	void hideEvent(QHideEvent *ev) override;

private:
	void buildUi();
	void applyState();
	void positionNearTray();
	void setupLayerShell();
	void fitWidthToHistory();
	void refreshList();
	void refreshContacts();
	void buildDialpad(QWidget *parent);

	/** Row widget for action buttons (add/edit/delete) placed
	 *  in the last column of the table. */
	QWidget *makeActionWidget(int row, bool isContact);

	/** Contacts view: split the spare table width equally
	 *  between the Name and Number/URI columns. */
	void distributeContactColumns();

	/** Contact add/edit overlay form. index >= 0 edits
	 *  contactEntries_[index]; index < 0 adds a new contact,
	 *  optionally prefilled from a history item's stored data. */
	void openContactForm(int index, QTableWidgetItem *prefill = nullptr);
	void loadContactsFile();
	void saveContactsFile();

	/** Delete-confirmation overlay (same style as the settings
	 *  account-removal overlay); runs `onYes` on Yes.
	 *  `noText` overrides the "No" button label (e.g. "Cancel"). */
	void confirmOverlay(const QString &text,
			    std::function<void()> onYes,
			    const QString &noText = "No");

	State state_;
	quintptr callPtr_ = 0;
	QString peerName_;
	bool isOutgoing_ = false;  /**< direction for InCall label */
	QSystemTrayIcon *trayIcon_ = nullptr;
	bool layerShellApplied_ = false;
	/** false = list shows call history, true = contacts. */
	bool showingContacts_ = false;
	QPoint anchorPos_;

	QFrame       *panel_         = nullptr;
	QLineEdit    *uriEdit_    = nullptr;
	QPushButton  *greenBtn_   = nullptr;
	QPushButton  *redBtn_     = nullptr;
	QWidget      *dialpadWidget_ = nullptr;
	QLineEdit    *dialpadLog_    = nullptr;
	QPushButton  *listToggleBtn_ = nullptr;
	QTabBar      *listTabs_     = nullptr;
	QTableWidget *historyList_ = nullptr;
	QWidget      *listGrip_    = nullptr;

	/* Bottom-left grip drag: resize panel width and height. */
	bool resizingPanel_    = false;
	QSize resizeStartSize_;
	QPoint resizeStartGlobal_;
	int  resizeStartTableH_ = 0;

	/* Contact add/edit overlay form + delete-confirm overlay. */
	QFrame    *formOverlay_   = nullptr;
	QFrame    *deleteOverlay_ = nullptr;
	QFrame    *importOverlay_ = nullptr;
	QLineEdit *cNameEdit_     = nullptr;
	QLineEdit *cNumEdit_      = nullptr;
	QComboBox *cTypeEdit_     = nullptr;
	int editingContact_ = -1;
	QList<ContactEntry> contactEntries_;
	QStringList           preservedLines_;
	std::function<void()> pendingDelete_;

private slots:
	void onGreen();
	void onRed();
	void sendDigit(char key);
	void onHistoryClicked(QTableWidgetItem *item);
	void onHistoryDoubleClicked(QTableWidgetItem *item);
};
