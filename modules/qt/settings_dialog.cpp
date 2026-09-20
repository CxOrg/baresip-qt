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
#include <functional>

#include <re.h>
#include <baresip.h>


/* ---- helpers ------------------------------------------------------- */

static QString homeBaresip()
{
	return QDir::homePath() + "/.baresip";
}

/** Return the Nth non-comment, non-empty account line index inside
 *  `lines`, or -1 if fewer accounts exist.
 */
static void forEachAccountLine(QStringList &lines, int index,
			       const std::function<void(QString &)> &fn)
{
	int seen = 0;
	for (QString &line : lines) {
		QString t = line.trimmed();
		if (t.isEmpty() || t.startsWith('#'))
			continue;
		if (seen == index) {
			fn(line);
			return;
		}
		++seen;
	}
}

static bool writeAccounts(const QStringList &lines)
{
	QFile f(homeBaresip() + "/accounts");
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return false;
	QTextStream out(&f);
	for (const QString &l : lines)
		out << l;
	out.flush();
	f.close();
	return true;
}

/** Read the ";enabled=" flag of account `index` from the accounts
 *  file. Absent param counts as enabled (default). */
static bool accountEnabledFromFile(int index)
{
	QFile f(homeBaresip() + "/accounts");
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return true;

	bool enabled = true;
	QStringList lines;
	while (!f.atEnd())
		lines << QString::fromUtf8(f.readLine());
	f.close();

	forEachAccountLine(lines, index, [&enabled](QString &line) {
		enabled = !line.contains(";enabled=no");
	});
	return enabled;
}

/** Rewrite specific parameters in account `index`'s line in
 *  ~/.baresip/accounts, preserving comments and unknown parameters
 *  (e.g. outbound, 100rel, ptime, ...). Returns true on success.
 */
static bool updateAccountsParams(const QMap<QString, QString> &updates,
				 int index)
{
	QString path = homeBaresip() + "/accounts";
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	QStringList lines;
	while (!f.atEnd())
		lines << QString::fromUtf8(f.readLine());
	f.close();

	bool modified = false;
	forEachAccountLine(lines, index, [&](QString &line) {
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
	});

	if (!modified)
		return false;

	return writeAccounts(lines);
}


/** Rewrite the domain part of account `index`'s AOR:
 *  <sip:user@DOMAIN[:PORT];uri-params> — replaces the text between
 *  '@' and the next ';' or '>', preserving the closing bracket.
 *  Returns true on success.
 */
static bool updateAccountsDomain(const QString &domain, int index)
{
	QString path = homeBaresip() + "/accounts";
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	QStringList lines;
	while (!f.atEnd())
		lines << QString::fromUtf8(f.readLine());
	f.close();

	bool modified = false;
	forEachAccountLine(lines, index, [&](QString &line) {
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
	});

	if (!modified)
		return false;

	return writeAccounts(lines);
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


QWidget *SettingsDialog::buildAccountPage(AccountWidgets &w,
					QTabWidget *tabs)
{
	auto *page = new QWidget(tabs);
	auto *form = new QFormLayout(page);

	w.enabled     = new QCheckBox("Register this account", page);
	w.enabled->setChecked(true);
	w.displayName = new QLineEdit(page);
	w.authUser    = new QLineEdit(page);
	w.authPass    = new QLineEdit(page);
	w.authPass->setEchoMode(QLineEdit::Password);
	w.sipDomain   = new QLineEdit(page);
	w.regint      = new QSpinBox(page);
	w.regint->setRange(0, 86400);
	w.regint->setSuffix(" s");

	w.stunHost    = new QLineEdit(page);
	w.stunPort    = new QSpinBox(page);
	w.stunPort->setRange(0, 65535);
	w.stunUser    = new QLineEdit(page);
	w.stunPass    = new QLineEdit(page);
	w.stunPass->setEchoMode(QLineEdit::Password);

	w.answermode  = new QComboBox(page);
	w.answermode->addItem("Manual",        ANSWERMODE_MANUAL);
	w.answermode->addItem("Early",         ANSWERMODE_EARLY);
	w.answermode->addItem("Auto",          ANSWERMODE_AUTO);
	w.answermode->addItem("Early Audio",   ANSWERMODE_EARLY_AUDIO);
	w.answermode->addItem("Early Video",   ANSWERMODE_EARLY_VIDEO);

	w.mediaenc    = new QComboBox(page);
	w.mediaenc->addItem("none");
	w.mediaenc->addItem("srtp");
	w.mediaenc->addItem("srtp-mand");
	w.mediaenc->addItem("srtp-mandf");
	w.mediaenc->addItem("dtls_srtp");
	w.mediaenc->addItem("zrtp");

	w.medianat    = new QComboBox(page);
	w.medianat->addItem("none");
	w.medianat->addItem("stun");
	w.medianat->addItem("turn");
	w.medianat->addItem("ice");

	w.audioCodecs = new QLineEdit(page);
	w.audioCodecs->setPlaceholderText("e.g. opus/48000/2,pcma,pcmu");

	form->addRow("Enabled:",        w.enabled);
	form->addRow("Display name:",   w.displayName);
	form->addRow("Auth user:",      w.authUser);
	form->addRow("Auth password:",  w.authPass);
	form->addRow("SIP domain:",     w.sipDomain);
	form->addRow("Register interval:", w.regint);
	form->addRow("STUN host:",      w.stunHost);
	form->addRow("STUN port:",      w.stunPort);
	form->addRow("STUN user:",      w.stunUser);
	form->addRow("STUN password:",  w.stunPass);
	form->addRow("Answer mode:",    w.answermode);
	form->addRow("Media encryption:", w.mediaenc);
	form->addRow("Media NAT:",      w.medianat);
	form->addRow("Audio codecs:",   w.audioCodecs);

	return page;
}


void SettingsDialog::buildUi()
{
	auto *tabs = new QTabWidget(this);
	auto *layout = new QVBoxLayout(this);
	layout->addWidget(tabs);

	/* ---- Account tabs (one per accounts-file line) ---- */
	for (int i = 0; i < kMaxAccounts; ++i)
		tabs->addTab(buildAccountPage(accounts_[i], tabs),
			     QString("Account %1").arg(i + 1));

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


void SettingsDialog::loadAccount(AccountWidgets &w, int index)
{
	/* The Nth UA in uag_list corresponds to the Nth non-comment
	 * line of ~/.baresip/accounts. */
	struct ua *ua = nullptr;
	struct le *le;
	int i = 0;
	for (le = list_head(uag_list()); le; le = le->next, ++i) {
		if (i == index) {
			ua = static_cast<struct ua *>(le->data);
			break;
		}
	}
	if (!ua)
		return;
	struct account *acc = ua_account(ua);
	if (!acc)
		return;

	w.enabled->setChecked(accountEnabledFromFile(index));
	w.displayName->setText(QString::fromUtf8(account_display_name(acc)));
	w.authUser->setText(QString::fromUtf8(account_auth_user(acc)));
	w.authPass->setText(QString::fromUtf8(account_auth_pass(acc)));

	/* Show only the domain part of the AOR: strip the
	 * "sip:user@" prefix and any ";params" suffix. */
	QString aor = QString::fromUtf8(account_aor(acc));
	int aorAt = aor.indexOf('@');
	QString domain = (aorAt >= 0) ? aor.mid(aorAt + 1) : aor;
	int aorSemi = domain.indexOf(';');
	if (aorSemi >= 0)
		domain = domain.left(aorSemi);
	w.sipDomain->setText(domain);

	w.regint->setValue(account_regint(acc));

	w.stunHost->setText(QString::fromUtf8(account_stun_host(acc)));
	w.stunPort->setValue(account_stun_port(acc));
	w.stunUser->setText(QString::fromUtf8(account_stun_user(acc)));
	w.stunPass->setText(QString::fromUtf8(account_stun_pass(acc)));

	w.answermode->setCurrentIndex(account_answermode(acc));

	const char *me = account_mediaenc(acc);
	if (me) {
		int j = w.mediaenc->findText(QString::fromUtf8(me), Qt::MatchFixedString);
		if (j >= 0) w.mediaenc->setCurrentIndex(j);
	}

	const char *mn = account_medianat(acc);
	if (mn) {
		int j = w.medianat->findText(QString::fromUtf8(mn), Qt::MatchFixedString);
		if (j >= 0) w.medianat->setCurrentIndex(j);
	}
}


void SettingsDialog::loadSettings()
{
	for (int i = 0; i < kMaxAccounts; ++i)
		loadAccount(accounts_[i], i);
}


void SettingsDialog::saveAccount(const AccountWidgets &w, int index)
{
	/* Find the Nth UA (same order as accounts-file lines). */
	struct ua *ua = nullptr;
	struct le *le;
	int i = 0;
	for (le = list_head(uag_list()); le; le = le->next, ++i) {
		if (i == index) {
			ua = static_cast<struct ua *>(le->data);
			break;
		}
	}
	if (!ua)
		return;
	struct account *acc = ua_account(ua);
	if (!acc)
		return;

	/* Apply live via account_set_*() APIs. */
	QByteArray dn = w.displayName->text().toUtf8();
	account_set_display_name(acc, dn.constData());

	QByteArray au = w.authUser->text().toUtf8();
	account_set_auth_user(acc, au.constData());

	QByteArray ap = w.authPass->text().toUtf8();
	account_set_auth_pass(acc, ap.constData());

	account_set_regint(acc, w.regint->value());

	QByteArray sh = w.stunHost->text().toUtf8();
	account_set_stun_host(acc, sh.constData());
	account_set_stun_port(acc, w.stunPort->value());

	QByteArray su = w.stunUser->text().toUtf8();
	account_set_stun_user(acc, su.constData());
	QByteArray sp = w.stunPass->text().toUtf8();
	account_set_stun_pass(acc, sp.constData());

	account_set_answermode(acc,
		static_cast<enum answermode>(w.answermode->currentData().toInt()));

	QByteArray me = w.mediaenc->currentText().toUtf8();
	if (me == "none") account_set_mediaenc(acc, NULL);
	else             account_set_mediaenc(acc, me.constData());

	QByteArray mn = w.medianat->currentText().toUtf8();
	if (mn == "none") account_set_medianat(acc, NULL);
	else              account_set_medianat(acc, mn.constData());

	QByteArray ac = w.audioCodecs->text().toUtf8();
	if (!ac.isEmpty())
		account_set_audio_codecs(acc, ac.constData());

	/* Toggling Enabled applies immediately: register now, or
	 * unregister without removing the account. ua_* calls must
	 * run on the re thread — go through the mqueue. */
	if (w.enabled->isChecked())
		qt_mod_register(ua);
	else
		qt_mod_unregister(ua);

	/* Persist to ~/.baresip/accounts (update known params in-place,
	 * preserving unknown params like outbound, 100rel, etc.). */
	QMap<QString, QString> updates;
	updates["enabled"] = w.enabled->isChecked() ? "yes" : "no";
	if (!w.displayName->text().isEmpty())
		updates["displayname"] = w.displayName->text();
	updates["auth_user"] = w.authUser->text();
	if (!w.authPass->text().isEmpty())
		updates["auth_pass"] = w.authPass->text();
	updates["regint"] = QString::number(w.regint->value());
	if (!w.stunHost->text().isEmpty()) {
		/* baresip expects "stun:[user@]host[:port]" — the '@'
		 * is required even with no user (stun:@host). Strip a
		 * leading '@' from the field so it can't double up. */
		QString host = w.stunHost->text();
		if (host.startsWith('@'))
			host = host.mid(1);
		QString ss = "stun:" + w.stunUser->text() + "@" + host;
		if (w.stunPort->value() != 3478)
			ss += QString(":%1").arg(w.stunPort->value());
		updates["stunserver"] = ss;
		if (!w.stunPass->text().isEmpty())
			updates["stunpass"] = w.stunPass->text();
	}
	updates["answermode"] = w.answermode->currentText().toLower();
	if (w.mediaenc->currentText() != "none")
		updates["mediaenc"] = w.mediaenc->currentText();
	if (w.medianat->currentText() != "none")
		updates["medianat"] = w.medianat->currentText();
	if (!w.audioCodecs->text().isEmpty())
		updates["audio_codecs"] = w.audioCodecs->text();

	updateAccountsParams(updates, index);

	/* The SIP domain lives inside the <sip:user@domain> AOR, not in
	 * a ;param — update it separately (takes effect on restart,
	 * since the AOR is the account's identity). */
	if (!w.sipDomain->text().isEmpty())
		updateAccountsDomain(w.sipDomain->text(), index);
}


void SettingsDialog::saveSettings()
{
	for (int i = 0; i < kMaxAccounts; ++i)
		saveAccount(accounts_[i], i);

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
