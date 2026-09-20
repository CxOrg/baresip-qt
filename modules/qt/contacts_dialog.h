/**
 * @file qt/contacts_dialog.h Qt UI module -- contacts add/edit panel
 *
 * Non-modal panel (same LayerShellQt top-right overlay style as the
 * settings dialog) with two tabs:
 *  - Contacts: lists the contacts from ~/.baresip/contacts; clicking
 *    an entry opens an overlay form to edit Name and Number.
 *  - History: unique numbers from the call history (no duplicates,
 *    no timestamps) with a call count per number; clicking an entry
 *    opens the same overlay to create a contact.
 *
 * "Save Contact" rewrites ~/.baresip/contacts (re-read, sorted by
 * name, comments preserved), syncs baresip's in-memory contact list
 * on the re thread, and emits contactsSaved() so the tray menu can
 * repopulate the Dial-contact submenu.
 */
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QList>
#include <QPoint>

class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QStackedWidget;
class QTabWidget;

class ContactsDialog : public QDialog {
	Q_OBJECT

public:
	explicit ContactsDialog(QWidget *parent = nullptr);

	/** One parsed contact line. Public so the file-local parse/format
	 *  helpers in the .cpp can use it. */
	struct ContactEntry {
		QString name;    /* display name, may be empty */
		QString uri;     /* URI inside <...>, e.g. sip:user@host */
		QString params;  /* addr-params after '>', incl. ';'s */
	};

signals:
	/** Emitted after the contacts file was rewritten and the
	 *  in-memory contact list was queued for re-sync. */
	void contactsSaved();

public:
	/** Tray-icon click position used to anchor the panel; call
	 *  before show(). */
	void setAnchorPoint(const QPoint &pos);

private:

	void buildUi();
	void setupLayerShell();
	void reloadContacts();
	void reloadHistory();
	void saveContacts();

	QTabWidget     *tabs_         = nullptr;
	QStackedWidget *contactsStack_ = nullptr;
	QStackedWidget *historyStack_  = nullptr;
	QListWidget    *contactsList_  = nullptr;
	QListWidget    *historyList_   = nullptr;

	/* Edit forms (one per tab). */
	QLineEdit *cNameEdit_ = nullptr;
	QLineEdit *cNumEdit_  = nullptr;
	QLineEdit *hNameEdit_ = nullptr;
	QLineEdit *hNumEdit_  = nullptr;

	QList<ContactEntry> entries_;
	QStringList         preservedLines_; /* comments/blank lines */
	int editingIndex_ = -1;
	QPoint anchorPos_;

private slots:
	void onTabChanged(int index);
	void onContactClicked(QListWidgetItem *item);
	void onHistoryClicked(QListWidgetItem *item);
	void onSaveContactEdit();
	void onSaveHistoryContact();
	void onDeleteContact();
	void onCancelContactEdit();
	void onCancelHistoryEdit();
};
