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
 * Settings are applied live via the account_set_*() APIs and also
 * written back to ~/.baresip/accounts and ~/.baresip/config so they
 * persist across restarts.
 */
#pragma once

#include <QDialog>
#include <QString>
#include <QList>

class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QTabWidget;
class QPushButton;
class QWidget;

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit SettingsDialog(QWidget *parent = nullptr);

private:
	/** Number of account tabs shown in the dialog. */
	static constexpr int kMaxAccounts = 2;

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
	};

	void buildUi();
	QWidget *buildAccountPage(AccountWidgets &w, QTabWidget *tabs);
	void loadSettings();
	void loadAccount(AccountWidgets &w, int index);
	void saveSettings();
	void saveAccount(const AccountWidgets &w, int index);

	AccountWidgets accounts_[kMaxAccounts];

	/* Audio tab */
	QComboBox *audioSrc_     = nullptr;
	QComboBox *audioPlayer_  = nullptr;

private slots:
	void onApply();
	void onOk();
	void onCancel();
};
