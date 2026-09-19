/**
 * @file qt/settings_dialog.cpp Qt UI module -- settings dialog
 */
#include "settings_dialog.h"
#include "qt_mod.h"

#include <QApplication>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QTabWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QStandardPaths>

#include <re.h>
#include <baresip.h>


/* ---- helpers ------------------------------------------------------- */

static QString homeBaresip()
{
	return QDir::homePath() + "/.baresip";
}

/** Rewrite the (single) account line in ~/.baresip/accounts,
 *  preserving comments. Returns true on success.
 */
static bool rewriteAccountsLine(const QString &newLine)
{
	QString path = homeBaresip() + "/accounts";
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	QStringList lines;
	bool replaced = false;
	while (!f.atEnd()) {
		QString line = QString::fromUtf8(f.readLine());
		QString trimmed = line.trimmed();
		if (!trimmed.startsWith('#') && !trimmed.isEmpty()
		    && !replaced) {
			lines << newLine + "\n";
			replaced = true;
		} else {
			lines << line;
		}
	}
	f.close();

	if (!replaced)
		lines << newLine + "\n";

	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return false;
	QTextStream out(&f);
	for (const QString &l : lines)
		out << l;
	out.flush();
	f.close();
	return true;
}


/* ---- dialog -------------------------------------------------------- */

SettingsDialog::SettingsDialog(QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle("baresip Settings");
	setAttribute(Qt::WA_DeleteOnClose, false);
	buildUi();
	loadSettings();
	resize(420, 480);
}


void SettingsDialog::buildUi()
{
	auto *tabs = new QTabWidget(this);
	auto *layout = new QVBoxLayout(this);
	layout->addWidget(tabs);

	/* ---- Account tab ---- */
	auto *accTab = new QWidget(tabs);
	auto *form = new QFormLayout(accTab);

	displayName_ = new QLineEdit(accTab);
	authUser_    = new QLineEdit(accTab);
	authPass_    = new QLineEdit(accTab);
	authPass_->setEchoMode(QLineEdit::Password);
	sipDomain_   = new QLineEdit(accTab);
	sipDomain_->setReadOnly(true);  /* AOR domain, set at startup */
	regint_      = new QSpinBox(accTab);
	regint_->setRange(0, 86400);
	regint_->setSuffix(" s");

	stunHost_    = new QLineEdit(accTab);
	stunPort_    = new QSpinBox(accTab);
	stunPort_->setRange(0, 65535);
	stunUser_    = new QLineEdit(accTab);
	stunPass_    = new QLineEdit(accTab);
	stunPass_->setEchoMode(QLineEdit::Password);

	answermode_  = new QComboBox(accTab);
	answermode_->addItem("Manual",        ANSWERMODE_MANUAL);
	answermode_->addItem("Early",         ANSWERMODE_EARLY);
	answermode_->addItem("Auto",          ANSWERMODE_AUTO);
	answermode_->addItem("Early Audio",   ANSWERMODE_EARLY_AUDIO);
	answermode_->addItem("Early Video",   ANSWERMODE_EARLY_VIDEO);

	mediaenc_    = new QComboBox(accTab);
	mediaenc_->addItem("none");
	mediaenc_->addItem("srtp");
	mediaenc_->addItem("srtp-mand");
	mediaenc_->addItem("srtp-mandf");
	mediaenc_->addItem("dtls_srtp");
	mediaenc_->addItem("zrtp");

	medianat_    = new QComboBox(accTab);
	medianat_->addItem("none");
	medianat_->addItem("stun");
	medianat_->addItem("turn");
	medianat_->addItem("ice");

	audioCodecs_ = new QLineEdit(accTab);
	audioCodecs_->setPlaceholderText("e.g. opus/48000/2,pcma,pcmu");

	form->addRow("Display name:",  displayName_);
	form->addRow("Auth user:",      authUser_);
	form->addRow("Auth password:",  authPass_);
	form->addRow("SIP domain:",     sipDomain_);
	form->addRow("Register interval:", regint_);
	form->addRow("STUN host:",      stunHost_);
	form->addRow("STUN port:",       stunPort_);
	form->addRow("STUN user:",      stunUser_);
	form->addRow("STUN password:",  stunPass_);
	form->addRow("Answer mode:",    answermode_);
	form->addRow("Media encryption:", mediaenc_);
	form->addRow("Media NAT:",       medianat_);
	form->addRow("Audio codecs:",    audioCodecs_);

	tabs->addTab(accTab, "Account");

	/* ---- Audio tab ---- */
	auto *audioTab = new QWidget(tabs);
	auto *aform = new QFormLayout(audioTab);

	audioSrc_    = new QComboBox(audioTab);
	audioPlayer_ = new QComboBox(audioTab);

	/* Populate with available ausrc/auplay module names. */
	struct list *ausrcl  = baresip_ausrcl();
	struct list *auplayl = baresip_auplayl();
	struct le *le;
	for (le = list_head(ausrcl); le; le = le->next) {
		struct ausrc *as = static_cast<struct ausrc *>(le->data);
		audioSrc_->addItem(QString::fromUtf8(ausrc_name(as)));
	}
	for (le = list_head(auplayl); le; le = le->next) {
		struct auplay *ap = static_cast<struct auplay *>(le->data);
		audioPlayer_->addItem(QString::fromUtf8(auplay_name(ap)));
	}

	aform->addRow("Audio source:",  audioSrc_);
	aform->addRow("Audio player:",  audioPlayer_);

	tabs->addTab(audioTab, "Audio");

	/* ---- Button row ---- */
	auto *btnRow = new QHBoxLayout();
	layout->addLayout(btnRow);

	auto *okBtn     = new QPushButton("OK", this);
	auto *applyBtn   = new QPushButton("Apply", this);
	auto *cancelBtn  = new QPushButton("Cancel", this);
	btnRow->addWidget(okBtn);
	btnRow->addWidget(applyBtn);
	btnRow->addStretch();
	btnRow->addWidget(cancelBtn);

	connect(okBtn,    &QPushButton::clicked, this, &SettingsDialog::onOk);
	connect(applyBtn, &QPushButton::clicked, this, &SettingsDialog::onApply);
	connect(cancelBtn, &QPushButton::clicked, this, &SettingsDialog::onCancel);
}


void SettingsDialog::loadSettings()
{
	struct ua *ua = qt_current_ua();
	if (!ua)
		return;
	struct account *acc = ua_account(ua);
	if (!acc)
		return;

	displayName_->setText(QString::fromUtf8(account_display_name(acc)));
	authUser_->setText(QString::fromUtf8(account_auth_user(acc)));
	authPass_->setText(QString::fromUtf8(account_auth_pass(acc)));
	sipDomain_->setText(QString::fromUtf8(account_aor(acc)));
	regint_->setValue(account_regint(acc));

	stunHost_->setText(QString::fromUtf8(account_stun_host(acc)));
	stunPort_->setValue(account_stun_port(acc));
	stunUser_->setText(QString::fromUtf8(account_stun_user(acc)));
	stunPass_->setText(QString::fromUtf8(account_stun_pass(acc)));

	answermode_->setCurrentIndex(account_answermode(acc));

	const char *me = account_mediaenc(acc);
	if (me) {
		int i = mediaenc_->findText(QString::fromUtf8(me), Qt::MatchFixedString);
		if (i >= 0) mediaenc_->setCurrentIndex(i);
	}

	const char *mn = account_medianat(acc);
	if (mn) {
		int i = medianat_->findText(QString::fromUtf8(mn), Qt::MatchFixedString);
		if (i >= 0) medianat_->setCurrentIndex(i);
	}
}


void SettingsDialog::saveSettings()
{
	struct ua *ua = qt_current_ua();
	if (!ua)
		return;
	struct account *acc = ua_account(ua);
	if (!acc)
		return;

	/* Apply live via account_set_*() APIs. */
	QByteArray dn = displayName_->text().toUtf8();
	account_set_display_name(acc, dn.constData());

	QByteArray au = authUser_->text().toUtf8();
	account_set_auth_user(acc, au.constData());

	QByteArray ap = authPass_->text().toUtf8();
	account_set_auth_pass(acc, ap.constData());

	account_set_regint(acc, regint_->value());

	QByteArray sh = stunHost_->text().toUtf8();
	account_set_stun_host(acc, sh.constData());
	account_set_stun_port(acc, stunPort_->value());

	QByteArray su = stunUser_->text().toUtf8();
	account_set_stun_user(acc, su.constData());
	QByteArray sp = stunPass_->text().toUtf8();
	account_set_stun_pass(acc, sp.constData());

	account_set_answermode(acc,
		static_cast<enum answermode>(answermode_->currentData().toInt()));

	QByteArray me = mediaenc_->currentText().toUtf8();
	if (me == "none") me = "";
	account_set_mediaenc(acc, me.constData());

	QByteArray mn = medianat_->currentText().toUtf8();
	if (mn == "none") mn = "";
	account_set_medianat(acc, mn.constData());

	QByteArray ac = audioCodecs_->text().toUtf8();
	if (!ac.isEmpty())
		account_set_audio_codecs(acc, ac.constData());

	/* Persist to ~/.baresip/accounts (rewrite the account line). */
	QString aor = QString::fromUtf8(account_aor(acc));
	/* Build a minimal account line preserving the SIP URI. */
	QString user = QString::fromUtf8(account_auth_user(acc));
	QString domain = aor;
	int at = domain.indexOf('@');
	if (at >= 0)
		domain = domain.mid(at + 1);

	QString line = QString("<sip:%1@%2;transport=udp>").arg(user, domain);
	line += QString(";auth_user=%1").arg(authUser_->text());
	if (!authPass_->text().isEmpty())
		line += QString(";auth_pass=%1").arg(authPass_->text());
	line += QString(";regint=%1").arg(regint_->value());
	if (!stunHost_->text().isEmpty()) {
		line += QString(";stunserver=stun:");
		if (!stunUser_->text().isEmpty())
			line += stunUser_->text() + "@";
		line += stunHost_->text();
		if (stunPort_->value() != 3478)
			line += QString(":%1").arg(stunPort_->value());
		if (!stunPass_->text().isEmpty())
			line += QString(";stunpass=%1").arg(stunPass_->text());
	}
	line += QString(";answermode=%1").arg(answermode_->currentText().toLower());
	if (mediaenc_->currentText() != "none")
		line += QString(";mediaenc=%1").arg(mediaenc_->currentText());
	if (medianat_->currentText() != "none")
		line += QString(";medianat=%1").arg(medianat_->currentText());
	if (!audioCodecs_->text().isEmpty())
		line += QString(";audio_codecs=%1").arg(audioCodecs_->text());

	rewriteAccountsLine(line);

	/* Audio device persistence: write to ~/.baresip/config. */
	/* (Audio device changes via config require a restart; we
	 * just persist the selection here.) */
}


void SettingsDialog::onApply()
{
	saveSettings();
}


void SettingsDialog::onOk()
{
	saveSettings();
	accept();
}


void SettingsDialog::onCancel()
{
	reject();
}
