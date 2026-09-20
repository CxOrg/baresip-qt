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
#include <QFrame>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QProcess>
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
	/* Rounded corners like the call panel: the dialog surface is
	 * transparent and an inner #settingsPanel layer carries the
	 * themed fill, border and 8px radius. */
	setAttribute(Qt::WA_TranslucentBackground);
	setStyleSheet(
		"QFrame#settingsPanel {"
		"  background-color: palette(window);"
		"  border: 2px solid palette(dark);"
		"  border-radius: 8px; }");
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
	ls->setMargins(qtPanelMargins(anchorPos_));
	ls->setDesiredSize(size());
#endif
}


void SettingsDialog::setAnchorPoint(const QPoint &pos)
{
	anchorPos_ = pos;
#ifdef HAVE_LAYERSHELL
	QWindow *win = windowHandle();
	auto *ls = win ? LayerShellQt::Window::get(win) : nullptr;
	if (ls)
		ls->setMargins(qtPanelMargins(anchorPos_));
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
	/* Outer layout is a 2px frame around an inner QFrame panel —
	 * same structure as the call dialog. The dialog surface stays
	 * transparent; the panel carries the themed fill, border and
	 * rounded corners. */
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(2, 2, 2, 2);

	auto *panel = new QFrame(this);
	panel->setObjectName("settingsPanel");
	panel->setAutoFillBackground(true);
	outer->addWidget(panel);

	auto *layout = new QVBoxLayout(panel);
	auto *tabs = new QTabWidget(panel);
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
	if (accountLine(index).isEmpty())
		return;

	/* Persist to ~/.baresip/accounts (update known params in-place,
	 * preserving unknown params like outbound, 100rel, etc.). The
	 * app is restarted after saving, which re-parses the file and
	 * rebuilds all UAs — no live apply is needed here. */
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
	 * a ;param — update it separately. */
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


void SettingsDialog::restartApp()
{
	/* Restart the whole app: simplest reliable re-register. The
	 * detached shell waits for this process to exit (releasing
	 * the SIP socket) before launching the new instance, which
	 * re-parses the accounts file and rebuilds all UAs. */
	QString cmd = QString(
		"while kill -0 %1 2>/dev/null; do sleep 0.1; done;"
		" exec baresip").arg(QCoreApplication::applicationPid());
	QProcess::startDetached("sh", {"-c", cmd});
	qt_mod_quit();
}


void SettingsDialog::onApply()
{
	saveSettings();
	restartApp();
}


void SettingsDialog::onOk()
{
	saveSettings();
	restartApp();
}


void SettingsDialog::onCancel()
{
	reject();
}
