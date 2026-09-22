/**
 * @file qt/settings_dialog.h Qt UI module -- settings dialog
 *
 * Non-modal dialog for editing basic baresip settings:
 *  - Account 1 / Account 2: display name, auth user/pass, SIP domain,
 *    regint, STUN, answermode, mediaenc, medianat, audio codecs,
 *    enabled flag
 *  - Audio: input/output device
 *
 * Each account tab maps to the Nth non-comment line of
 * ~/.baresip/accounts. The per-account "Enabled" flag is stored as a
 * ";enabled={yes,no}" addr-param (ignored by baresip's parser, honored
 * by the qt module which unregisters disabled accounts at startup).
 *
 * Settings are written back to ~/.baresip/accounts, then applied
 * in-process: a changed account's UA is destroyed and recreated from
 * the new accounts line (a fresh reg client avoids the stuck
 * "unregistering" state), and the tray rebuilds its account menu on
 * UA create/destroy.
 */
#pragma once

#include <QDialog>
#include <QString>
#include <QList>
#include <QPoint>

class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QTabWidget;
class QToolButton;
class QPushButton;
class QWidget;

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit SettingsDialog(QWidget *parent = nullptr);

	/** Tray-icon click position used to anchor the panel; call
	 *  before show(). */
	void setAnchorPoint(const QPoint &pos);

private:
	/** Maximum number of account tabs (the + button hides at this
	 *  count). */
	static constexpr int kMaxAccounts = 4;

	/** All input widgets of one account tab. */
	struct AccountWidgets {
		QCheckBox *enabled     = nullptr;
		QLineEdit *displayName = nullptr;
		QLineEdit *authUser    = nullptr;
		QLineEdit *authPass    = nullptr;
		QLineEdit *sipDomain   = nullptr;
		QSpinBox  *regint      = nullptr;
		QLineEdit *stunHost    = nullptr;
		QSpinBox  *stunPort    = nullptr;
		QLineEdit *stunUser    = nullptr;
		QLineEdit *stunPass    = nullptr;
		QComboBox *answermode  = nullptr;
		QComboBox *mediaenc    = nullptr;
		QComboBox *medianat    = nullptr;
		QLineEdit *audioCodecs = nullptr;

		/** Accounts-file line as it was at load time; used to
		 *  detect changes and to find the live UA by AOR. */
		QString origLine;
	};

	void buildUi();
	void setupLayerShell();
	QWidget *buildAccountPage(AccountWidgets &w, QTabWidget *tabs);
	void loadSettings();
	void loadAccount(AccountWidgets &w, int index);
	void saveSettings();
	void saveAccount(AccountWidgets &w, int index);
	void onAddAccount();
	void onRemoveAccount();
	void updateAddTabVisibility();
	void updateRemoveTabVisibility();

	AccountWidgets accounts_[kMaxAccounts];

	QTabWidget *tabs_          = nullptr;
	QToolButton *addTabBtn_    = nullptr;
	QToolButton *removeTabBtn_ = nullptr;

	/* Audio tab */
	QComboBox *audioSrc_     = nullptr;
	QComboBox *audioPlayer_  = nullptr;

	QPoint anchorPos_;

private slots:
	void onApply();
	void onOk();
	void onCancel();
};
