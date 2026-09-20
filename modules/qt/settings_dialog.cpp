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
#include <QWindow>
#include <functional>

#ifdef HAVE_LAYERSHELL
#include <LayerShellQt/Window>
#endif

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

/** Return the Nth non-comment, non-empty account line, or empty. */
static QString accountLine(int index)
{
	QFile f(homeBaresip() + "/accounts");
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return QString();

	QString found;
	QStringList lines;
	while (!f.atEnd())
		lines << QString::fromUtf8(f.readLine());
	f.close();

	forEachAccountLine(lines, index, [&found](QString &line) {
		found = line;
	});
	return found;
}

/** Extract ";key=value" from an account line. Addr-params live after
 *  the closing '>' of <sip:...> so the search starts there; a value
 *  ends at the nearest ';' or end-of-line. Quotes are stripped. */
static QString lineParam(const QString &line, const QString &key)
{
	int gt = line.lastIndexOf('>');
	QString param = QString(";%1=").arg(key);
	int pi = line.indexOf(param, gt >= 0 ? gt : 0);
	if (pi < 0)
		return QString();

	int vi = pi + param.length();
	int end = line.indexOf('\n', vi);
	if (end < 0) end = line.length();
	int semi = line.indexOf(';', vi);
	if (semi >= 0 && semi < end)
		end = semi;

	QString v = line.mid(vi, end - vi).trimmed();
	if (v.startsWith('"') && v.endsWith('"') && v.length() > 1)
		v = v.mid(1, v.length() - 2);
	return v;
}

/** "user@host[:port]" extracted from the line's <sip:...> AOR,
 *  for matching against account_aor() of a live UA. */
static QString lineAorUserHost(const QString &line)
{
	int lt = line.indexOf('<');
	int gt = line.indexOf('>', lt);
	if (lt < 0 || gt < 0)
		return QString();
	QString aor = line.mid(lt + 1, gt - lt - 1);
	int semi = aor.indexOf(';');
	if (semi >= 0)
		aor = aor.left(semi);
	if (aor.startsWith("sip:", Qt::CaseInsensitive))
		aor = aor.mid(4);
	else if (aor.startsWith("sips:", Qt::CaseInsensitive))
		aor = aor.mid(5);
	return aor.trimmed();
}

static QString displayNameFromLine(const QString &line)
{
	QString d = lineParam(line, "displayname");
	if (!d.isEmpty())
		return d;
	/* Fall back to a quoted "Name" prefix before the <sip:...>. */
	int lt = line.indexOf('<');
	if (lt > 0) {
		QString pre = line.left(lt).trimmed();
		if (pre.startsWith('"') && pre.endsWith('"')
		    && pre.length() > 1)
			pre = pre.mid(1, pre.length() - 2);
		return pre;
	}
	return QString();
}

static int answermodeFromString(const QString &s)
{
	if (s == "early")       return ANSWERMODE_EARLY;
	if (s == "auto")        return ANSWERMODE_AUTO;
	if (s == "early-audio") return ANSWERMODE_EARLY_AUDIO;
	if (s == "early-video") return ANSWERMODE_EARLY_VIDEO;
	return ANSWERMODE_MANUAL;
}

static QString answermodeToString(int mode)
{
	switch (mode) {
	case ANSWERMODE_EARLY:       return "early";
	case ANSWERMODE_AUTO:        return "auto";
	case ANSWERMODE_EARLY_AUDIO: return "early-audio";
	case ANSWERMODE_EARLY_VIDEO: return "early-video";
	default:                     return "manual";
	}
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
	resize(470, 480);
	setupLayerShell();
}


void SettingsDialog::setupLayerShell()
{
#ifdef HAVE_LAYERSHELL
	/* Same pattern as CallDialog: create a native handle, destroy
	 * the xdg-shell surface so LayerShellQt can intercept surface
	 * creation at show(), then anchor top-right on the Overlay
	 * layer — the same screen location as the call panel. */
	setAttribute(Qt::WA_NativeWindow);
	winId();

	QWindow *win = windowHandle();
	if (!win)
		return;

	win->destroy();

	auto *ls = LayerShellQt::Window::get(win);
	if (!ls)
		return;

	ls->setLayer(LayerShellQt::Window::LayerOverlay);
	ls->setKeyboardInteractivity(
		LayerShellQt::Window::KeyboardInteractivityOnDemand);
	ls->setScope("baresip-settings");
	ls->setAnchors(LayerShellQt::Window::Anchors(
		LayerShellQt::Window::AnchorTop |
		LayerShellQt::Window::AnchorRight));
	ls->setMargins(QMargins(0, 58, 8, 0));
	ls->setDesiredSize(size());
#endif
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
	/* Parse the accounts-file line directly — the tab must show
	 * configured accounts even when they have no UA (disabled
	 * accounts are destroyed at startup, not merely
	 * unregistered). */
	QString line = accountLine(index);
	if (line.isEmpty())
		return;

	w.enabled->setChecked(!line.contains(";enabled=no"));
	w.displayName->setText(displayNameFromLine(line));
	w.authUser->setText(lineParam(line, "auth_user"));
	w.authPass->setText(lineParam(line, "auth_pass"));

	/* SIP domain: between '@' and the next ';' or '>' inside <>. */
	int at = line.indexOf('@');
	int gt = line.indexOf('>', at);
	if (at >= 0 && gt > at) {
		int end = gt;
		int semi = line.indexOf(';', at);
		if (semi >= 0 && semi < end)
			end = semi;
		w.sipDomain->setText(line.mid(at + 1, end - at - 1));
	}

	QString ri = lineParam(line, "regint");
	w.regint->setValue(ri.isEmpty() ? 3600 : ri.toInt());

	/* stunserver=stun:[user@]host[:port] */
	QString ss = lineParam(line, "stunserver");
	if (ss.startsWith("stun:"))
		ss = ss.mid(5);
	int ssAt = ss.lastIndexOf('@');
	QString hostPort = (ssAt >= 0) ? ss.mid(ssAt + 1) : ss;
	QString user = (ssAt > 0) ? ss.left(ssAt) : QString();
	QString port;
	int colon = hostPort.lastIndexOf(':');
	if (colon >= 0) {
		port = hostPort.mid(colon + 1);
		hostPort = hostPort.left(colon);
	}
	w.stunHost->setText(hostPort);
	if (!port.isEmpty())
		w.stunPort->setValue(port.toInt());
	QString su = lineParam(line, "stunuser");
	w.stunUser->setText(su.isEmpty() ? user : su);
	w.stunPass->setText(lineParam(line, "stunpass"));

	w.answermode->setCurrentIndex(
		answermodeFromString(lineParam(line, "answermode")));

	QString me = lineParam(line, "mediaenc");
	if (!me.isEmpty()) {
		int j = w.mediaenc->findText(me, Qt::MatchFixedString);
		if (j >= 0) w.mediaenc->setCurrentIndex(j);
	}

	QString mn = lineParam(line, "medianat");
	if (!mn.isEmpty()) {
		int j = w.medianat->findText(mn, Qt::MatchFixedString);
		if (j >= 0) w.medianat->setCurrentIndex(j);
	}

	w.audioCodecs->setText(lineParam(line, "audio_codecs"));
}


void SettingsDialog::loadSettings()
{
	for (int i = 0; i < kMaxAccounts; ++i)
		loadAccount(accounts_[i], i);
}


void SettingsDialog::saveAccount(const AccountWidgets &w, int index)
{
	/* Find the UA matching this account line by AOR — index-based
	 * mapping breaks once disabled accounts are destroyed and the
	 * remaining UAs shift position. */
	QString origLine = accountLine(index);
	if (origLine.isEmpty())
		return;

	QString wantAor = lineAorUserHost(origLine);
	struct ua *ua = nullptr;
	struct le *le;
	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *u = static_cast<struct ua *>(le->data);
		QString have = QString::fromUtf8(
			account_aor(ua_account(u)));
		if (have.startsWith("sip:", Qt::CaseInsensitive))
			have = have.mid(4);
		else if (have.startsWith("sips:", Qt::CaseInsensitive))
			have = have.mid(5);
		if (have == wantAor) {
			ua = u;
			break;
		}
	}

	/* Apply live via account_set_*() APIs — only when the account
	 * actually has a UA (disabled accounts have none). */
	struct account *acc = ua ? ua_account(ua) : nullptr;
	if (acc) {
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
	}

	/* Persist to ~/.baresip/accounts (update known params in-place,
	 * preserving unknown params like outbound, 100rel, etc.). Runs
	 * even when the account has no live UA. */
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
	updates["answermode"] = answermodeToString(
		w.answermode->currentData().toInt());
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

	/* Enabled transitions — ua_* / mem_deref must run on the re
	 * thread, so they all go through the mqueue. */
	if (ua) {
		if (w.enabled->isChecked())
			qt_mod_register(ua);
		else
			qt_mod_ua_free(ua);   /* unload, like startup */
	}
	else if (w.enabled->isChecked()) {
		/* No UA (account was disabled at startup): recreate it
		 * from the just-written accounts-file line. ua_alloc
		 * registers itself when regint > 0. */
		QString newLine = accountLine(index);
		if (!newLine.isEmpty())
			qt_mod_ua_alloc(newLine);
	}
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
