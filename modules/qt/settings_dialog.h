/**
 * @file qt/settings_dialog.h Qt UI module -- settings dialog
 *
 * Non-modal dialog for editing basic baresip settings:
 *  - Account: display name, auth user/pass, regint, STUN, answermode,
 *    mediaenc, medianat, audio codecs
 *  - Audio: input/output device
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

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit SettingsDialog(QWidget *parent = nullptr);

private:
	void buildUi();
	void loadSettings();
	void saveSettings();

	/* Account tab */
	QLineEdit *displayName_  = nullptr;
	QLineEdit *authUser_     = nullptr;
	QLineEdit *authPass_     = nullptr;
	QLineEdit *sipDomain_    = nullptr;
	QSpinBox  *regint_       = nullptr;
	QLineEdit *stunHost_     = nullptr;
	QSpinBox  *stunPort_     = nullptr;
	QLineEdit *stunUser_     = nullptr;
	QLineEdit *stunPass_     = nullptr;
	QComboBox *answermode_   = nullptr;
	QComboBox *mediaenc_     = nullptr;
	QComboBox *medianat_     = nullptr;
	QLineEdit *audioCodecs_  = nullptr;

	/* Audio tab */
	QComboBox *audioSrc_     = nullptr;
	QComboBox *audioPlayer_  = nullptr;

private slots:
	void onApply();
	void onOk();
	void onCancel();
};
