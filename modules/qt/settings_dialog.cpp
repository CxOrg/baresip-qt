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

/** Rewrite specific parameters in the (single) account line in
 *  ~/.baresip/accounts, preserving comments and unknown parameters
 *  (e.g. outbound, 100rel, ptime, ...). Returns true on success.
 */
static bool updateAccountsParams(const QMap<QString, QString> &updates)
{
	QString path = homeBaresip() + "/accounts";
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	QStringList lines;
	bool modified = false;
	while (!f.atEnd()) {
		QString line = QString::fromUtf8(f.readLine());
		QString trimmed = line.trimmed();
		if (!trimmed.startsWith('#') && !trimmed.isEmpty()
		    && !modified) {
			/* Apply each update to this line. */
			for (auto it = updates.begin(); it != updates.end(); ++it) {
				const QString &key = it.key();
				const QString &val = it.value();
				QString param = QString(";%1=").arg(key);
				int pi = line.indexOf(param);
				if (pi >= 0) {
					/* Replace existing value. The value ends at
					 * the NEAREST of ';', '>' or end-of-line —
					 * scanning past '>' would eat the closing
					 * bracket of the <sip:...> address. */
					int vi = pi + param.length();
					int end = line.indexOf('\n', vi);
					if (end < 0) end = line.length();
					for (const char c : {';', '>'}) {
						int i = line.indexOf(c, vi);
						if (i >= 0 && i < end)
							end = i;
					}
					QString oldVal = line.mid(vi, end - vi);
					QString quoted = oldVal.startsWith('"') ? oldVal : QString();
					QString newVal = val;
					if (quoted.startsWith('"')) newVal = QString("\"%1\"").arg(val);
					line.replace(vi, end - vi, newVal);
				} else if (!val.isEmpty()) {
					/* Append as an addr-param AFTER the
					 * closing '>' — inserting before it
					 * would put the param inside the
					 * <sip:...> URI brackets. */
					int gt = line.lastIndexOf('>');
					if (gt >= 0)
						line.insert(gt + 1, QString(";%1=%2").arg(key, val));
				}
			}
			modified = true;
		}
		lines << line;
	}
	f.close();

	if (!modified)
		return false;

	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return false;
	QTextStream out(&f);
	for (const QString &l : lines)
		out << l;
	out.flush();
	f.close();
	return true;
}


/** Rewrite the domain part of the AOR in the (single) account line:
 *  <sip:user@DOMAIN[:PORT];uri-params> — replaces the text between
 *  '@' and the next ';' or '>', preserving the closing bracket.
 *  Returns true on success.
 */
static bool updateAccountsDomain(const QString &domain)
{
	QString path = homeBaresip() + "/accounts";
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	QStringList lines;
	bool modified = false;
	while (!f.atEnd()) {
		QString line = QString::fromUtf8(f.readLine());
		QString trimmed = line.trimmed();
		if (!trimmed.startsWith('#') && !trimmed.isEmpty()
		    && !modified) {
			int lt = line.indexOf('<');
			int at = line.indexOf('@', lt);
			int gt = line.indexOf('>', at);
			if (lt >= 0 && at > lt && gt > at) {
				int end = gt;
				int semi = line.indexOf(';', at);
				if (semi >= 0 && semi < end)
					end = semi;
				line.replace(at + 1, end - at - 1, domain);
				modified = true;
			}
		}
		lines << line;
	}
	f.close();

	if (!modified)
		return false;

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
		audioSrc_->addItem(QString::fromUtf8(as->name));
	}
	for (le = list_head(auplayl); le; le = le->next) {
		struct auplay *ap = static_cast<struct auplay *>(le->data);
		audioPlayer_->addItem(QString::fromUtf8(ap->name));
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
	/* Show only the domain part of the AOR: strip the
	 * "sip:user@" prefix and any ";params" suffix. */
	QString aor = QString::fromUtf8(account_aor(acc));
	int aorAt = aor.indexOf('@');
	QString domain = (aorAt >= 0) ? aor.mid(aorAt + 1) : aor;
	int aorSemi = domain.indexOf(';');
	if (aorSemi >= 0)
		domain = domain.left(aorSemi);
	sipDomain_->setText(domain);
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
	if (me == "none") account_set_mediaenc(acc, NULL);
	else             account_set_mediaenc(acc, me.constData());

	QByteArray mn = medianat_->currentText().toUtf8();
	if (mn == "none") account_set_medianat(acc, NULL);
	else              account_set_medianat(acc, mn.constData());

	QByteArray ac = audioCodecs_->text().toUtf8();
	if (!ac.isEmpty())
		account_set_audio_codecs(acc, ac.constData());

	/* Persist to ~/.baresip/accounts (update known params in-place,
	 * preserving unknown params like outbound, 100rel, etc.). */
	QMap<QString, QString> updates;
	if (!displayName_->text().isEmpty())
		updates["displayname"] = displayName_->text();
	updates["auth_user"] = authUser_->text();
	if (!authPass_->text().isEmpty())
		updates["auth_pass"] = authPass_->text();
	updates["regint"] = QString::number(regint_->value());
	if (!stunHost_->text().isEmpty()) {
		/* baresip expects "stun:[user@]host[:port]" — the '@'
		 * is required even with no user (stun:@host). Strip a
		 * leading '@' from the field so it can't double up. */
		QString host = stunHost_->text();
		if (host.startsWith('@'))
			host = host.mid(1);
		QString ss = "stun:" + stunUser_->text() + "@" + host;
		if (stunPort_->value() != 3478)
			ss += QString(":%1").arg(stunPort_->value());
		updates["stunserver"] = ss;
		if (!stunPass_->text().isEmpty())
			updates["stunpass"] = stunPass_->text();
	}
	updates["answermode"] = answermode_->currentText().toLower();
	if (mediaenc_->currentText() != "none")
		updates["mediaenc"] = mediaenc_->currentText();
	if (medianat_->currentText() != "none")
		updates["medianat"] = medianat_->currentText();
	if (!audioCodecs_->text().isEmpty())
		updates["audio_codecs"] = audioCodecs_->text();

	updateAccountsParams(updates);

	/* The SIP domain lives inside the <sip:user@domain> AOR, not in
	 * a ;param — update it separately (takes effect on restart,
	 * since the AOR is the account's identity). */
	if (!sipDomain_->text().isEmpty())
		updateAccountsDomain(sipDomain_->text());

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
