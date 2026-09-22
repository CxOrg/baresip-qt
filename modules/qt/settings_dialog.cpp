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
#include <QToolButton>
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
#include <QResizeEvent>
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

/** Extract "user@host[:port]" from an AOR string — works on both
 *  accounts-file lines and account_aor() output: keeps the URI
 *  inside <...>, strips the scheme and any ;params. */
static QString userHostFromAor(QString aor)
{
	int lt = aor.indexOf('<');
	int gt = aor.indexOf('>');
	if (lt >= 0 && gt > lt)
		aor = aor.mid(lt + 1, gt - lt - 1);
	if (aor.startsWith("sip:"))
		aor = aor.mid(4);
	else if (aor.startsWith("sips:"))
		aor = aor.mid(5);
	int semi = aor.indexOf(';');
	if (semi >= 0)
		aor = aor.left(semi);
	return aor.trimmed();
}


/** Find the live UA whose account AOR matches an accounts-file
 *  line's user@host. Returns nullptr when the account has no UA
 *  (disabled accounts are destroyed at startup). */
static struct ua *findUaByUserHost(const QString &line)
{
	QString want = userHostFromAor(line);
	if (want.isEmpty())
		return nullptr;

	for (struct le *le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = static_cast<struct ua *>(le->data);
		const char *aor = account_aor(ua_account(ua));
		if (aor && userHostFromAor(QString::fromUtf8(aor)) == want)
			return ua;
	}
	return nullptr;
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


/** Remove a ";key=value" addr-param from an account line
 *  (params after the closing '>'). Returns the modified line. */
static QString removeLineParam(const QString &line, const QString &key)
{
	int gt = line.lastIndexOf('>');
	int start = gt >= 0 ? gt : 0;
	QString param = QString(";%1=").arg(key);
	int pi = line.indexOf(param, start);
	if (pi < 0)
		return line;
	int vi = pi + param.length();
	int end = line.indexOf(';', vi);
	int nl = line.indexOf('\n', vi);
	if (end < 0 || (nl >= 0 && nl < end))
		end = nl;
	if (end < 0)
		end = line.length();
	return line.left(pi) + line.mid(end);
}

/** Count non-comment, non-empty account lines in the accounts file. */
static int countAccountLines()
{
	QFile f(homeBaresip() + "/accounts");
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return 0;
	int count = 0;
	while (!f.atEnd()) {
		QString t = QString::fromUtf8(f.readLine()).trimmed();
		if (!t.isEmpty() && !t.startsWith('#'))
			++count;
	}
	f.close();
	return count;
}

/** Append a new account line to ~/.baresip/accounts. */
static bool appendAccountLine(const QString &line)
{
	QFile f(homeBaresip() + "/accounts");
	if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
		return false;
	QTextStream out(&f);
	out << line;
	if (!line.endsWith('\n'))
		out << '\n';
	out.flush();
	f.close();
	return true;
}

/** Remove the Nth non-comment, non-empty account line from
 *  ~/.baresip/accounts. Returns true on success. */
static bool removeAccountLine(int index)
{
	QString path = homeBaresip() + "/accounts";
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	QStringList lines;
	while (!f.atEnd())
		lines << QString::fromUtf8(f.readLine());
	f.close();

	bool removed = false;
	int seen = 0;
	for (int i = 0; i < lines.size(); ++i) {
		QString t = lines[i].trimmed();
		if (t.isEmpty() || t.startsWith('#'))
			continue;
		if (seen == index) {
			lines.removeAt(i);
			removed = true;
			break;
		}
		++seen;
	}

	if (!removed)
		return false;

	return writeAccounts(lines);
}

/** Build a blank account line from the last account entry: copy
 *  the structure/params (transport, regint, STUN, mediaenc, etc.)
 *  but blank the display name, auth user, auth pass, and the AOR
 *  user@domain. The new account is disabled by default. */
static QString blankAccountFromLast(const QString &lastLine)
{
	QString line = lastLine;

	/* Remove quoted display name prefix before <. */
	int lt = line.indexOf('<');
	if (lt > 0) {
		QString pre = line.left(lt).trimmed();
		if (pre.startsWith('"') && pre.endsWith('"'))
			line = line.mid(lt);
	}

	/* Remove params that should be blank. */
	line = removeLineParam(line, "displayname");
	line = removeLineParam(line, "auth_user");
	line = removeLineParam(line, "auth_pass");
	line = removeLineParam(line, "enabled");

	/* Replace user@domain in <sip:user@domain...> with @. */
	lt = line.indexOf('<');
	if (lt >= 0) {
		int sip = line.indexOf("sip:", lt);
		if (sip >= 0) {
			int at = line.indexOf('@', sip);
			int gt = line.indexOf('>', sip);
			if (at > sip && gt > at) {
				int end = gt;
				int semi = line.indexOf(';', at);
				if (semi >= 0 && semi < end)
					end = semi;
				line = line.left(sip + 4) + "@" + line.mid(end);
			}
		}
	}

	/* Disable the new account by default. */
	int gt = line.lastIndexOf('>');
	if (gt >= 0)
		line.insert(gt + 1, ";enabled=no");

	return line;
}

/** Rewrite the user part of account `index`'s AOR:
 *  <sip:USER@domain[:port];uri-params>. Returns true on success. */
static bool updateAccountsUser(const QString &user, int index)
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
		int sip = line.indexOf("sip:", lt);
		int at = line.indexOf('@', sip);
		if (lt >= 0 && sip > lt && at > sip) {
			line.replace(sip + 4, at - sip - 4, user);
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
		"  border-radius: 8px; }"
		/* Input fields: zero padding (overrides Qt6Curve theme
		 * padding that pushes text off-center) and a small
		 * min-height for a slightly taller field. */
		"QLineEdit, QSpinBox, QComboBox {"
		"  padding: 0px 0px 1px 0px; min-height: 1.0em; }");
	buildUi();
	loadSettings();
	resize(470, 480);
	setupLayerShell();
}


void SettingsDialog::resizeEvent(QResizeEvent *ev)
{
	QDialog::resizeEvent(ev);
	if (confirmOverlay_ && panel_) {
		int mw = panel_->width() / 10;
		int mh = panel_->height() * 3 / 10;
		confirmOverlay_->setGeometry(mw, mh,
			panel_->width() - 2 * mw,
			panel_->height() - 2 * mh);
	}
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
	qtPanelApplyAnchors(win, anchorPos_);
	ls->setDesiredSize(size());
#endif
}


void SettingsDialog::setAnchorPoint(const QPoint &pos)
{
	anchorPos_ = pos;
	qtPanelApplyAnchors(windowHandle(), anchorPos_);
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
	w.sipDomain->setPlaceholderText("[sipdomain]:[port]");
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
	panel_ = panel;

	auto *layout = new QVBoxLayout(panel);
	auto *tabs = new QTabWidget(panel);
	layout->addWidget(tabs);
	tabs_ = tabs;

	/* ---- Account tabs (one per accounts-file line) ---- */
	int nAccounts = qMin(countAccountLines(), kMaxAccounts);
	for (int i = 0; i < nAccounts; ++i)
		tabs->addTab(buildAccountPage(accounts_[i], tabs),
			     QString("Account %1").arg(i + 1));

	/* ---- "+"/"-" buttons in the tab header row (top-right corner) ---- */
	auto *corner = new QWidget(tabs);
	auto *cornerLay = new QHBoxLayout(corner);
	cornerLay->setContentsMargins(0, 0, 0, 0);
	cornerLay->setSpacing(4);

	/* Simple +/- symbols in button-like areas with a mid-grey
	 * border. Font forced to regular (non-italic), button size
	 * reduced 20% to 24x24, 3px bottom margin. */
	addTabBtn_ = new QToolButton(corner);
	addTabBtn_->setText("+");
	addTabBtn_->setFixedSize(24, 24);
	addTabBtn_->setToolTip("Add account then save");
	addTabBtn_->setAutoRaise(false);
	addTabBtn_->setStyleSheet(
		"QToolButton { border: 1px solid #808080; border-radius: 4px;"
		"              font-size: 19px; font-weight: bold;"
		"              font-style: normal; padding: 0;"
		"              margin-bottom: 3px; }"
		"QToolButton:hover { border: 1px solid #808080;"
		"                    background: #c0c0c0;"
		"                    margin-bottom: 3px; }");
	cornerLay->addWidget(addTabBtn_);

	removeTabBtn_ = new QToolButton(corner);
	removeTabBtn_->setText(QString::fromUtf8("\xe2\x88\x92"));
	removeTabBtn_->setFixedSize(24, 24);
	removeTabBtn_->setToolTip("Remove account and confirm");
	removeTabBtn_->setAutoRaise(false);
	removeTabBtn_->setStyleSheet(
		"QToolButton { border: 1px solid #808080; border-radius: 4px;"
		"              font-size: 19px; font-weight: bold;"
		"              font-style: normal; padding: 0;"
		"              margin-bottom: 3px; }"
		"QToolButton:hover { border: 1px solid #808080;"
		"                    background: #c0c0c0;"
		"                    margin-bottom: 3px; }");
	cornerLay->addWidget(removeTabBtn_);

	tabs->setCornerWidget(corner, Qt::TopRightCorner);
	connect(addTabBtn_, &QPushButton::clicked,
		this, &SettingsDialog::onAddAccount);
	connect(removeTabBtn_, &QPushButton::clicked,
		this, &SettingsDialog::onRemoveAccount);
	connect(tabs, &QTabWidget::currentChanged,
		this, &SettingsDialog::updateRemoveTabVisibility);

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
	updateAddTabVisibility();

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

	w.origLine = line;
	w.enabled->setChecked(!line.contains(";enabled=no"));
	w.displayName->setText(displayNameFromLine(line));
	/* Auth user: the explicit ;auth_user= param, or the AOR
	 * user part — baresip authenticates with the user part
	 * when no auth_user param is set. */
	QString au = lineParam(line, "auth_user");
	if (au.isEmpty()) {
		QString uh = userHostFromAor(line);
		au = uh.left(uh.indexOf('@'));
	}
	w.authUser->setText(au);
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


void SettingsDialog::saveAccount(AccountWidgets &w, int index)
{
	QString existingLine = accountLine(index);

	if (existingLine.isEmpty()) {
		/* New account added via the "+" button: build a template
		 * from the duplicated origLine and append it, then fall
		 * through to the normal param-update path. Skip if the
		 * user hasn't filled in the domain yet. */
		QString sd = w.sipDomain->text().trimmed();
		if (sd.isEmpty() || sd == "[sipdomain]:[port]")
			return;

		QString baseLine = w.origLine;
		if (baseLine.isEmpty())
			baseLine = "<sip:@" + sd + ">";
		else
			baseLine = blankAccountFromLast(baseLine);

		appendAccountLine(baseLine);

		/* Set the AOR user part from the auth user field. */
		QString au = w.authUser->text().trimmed();
		if (!au.isEmpty())
			updateAccountsUser(au, index);
	}

	/* Persist to ~/.baresip/accounts (update known params in-place,
	 * preserving unknown params like outbound, 100rel, etc.), then
	 * apply in-process below. */
	/* Trim leading/trailing whitespace from every field value;
	 * display name keeps internal spaces. */
	QString displayName = w.displayName->text().trimmed();
	QString authUser    = w.authUser->text().trimmed();
	QString authPass    = w.authPass->text().trimmed();
	QString stunHost    = w.stunHost->text().trimmed();
	QString stunUser   = w.stunUser->text().trimmed();
	QString stunPass   = w.stunPass->text().trimmed();
	QString audioCodecs= w.audioCodecs->text().trimmed();
	QString sipDomain  = w.sipDomain->text().trimmed();

	QMap<QString, QString> updates;
	updates["enabled"] = w.enabled->isChecked() ? "yes" : "no";
	if (!displayName.isEmpty())
		updates["displayname"] = displayName;
	if (!authUser.isEmpty())
		updates["auth_user"] = authUser;
	if (!authPass.isEmpty())
		updates["auth_pass"] = authPass;
	updates["regint"] = QString::number(w.regint->value());
	if (!stunHost.isEmpty()) {
		/* baresip expects "stun:[user@]host[:port]" — the '@'
		 * is required even with no user (stun:@host). Strip a
		 * leading '@' from the field so it can't double up. */
		QString host = stunHost;
		if (host.startsWith('@'))
			host = host.mid(1);
		QString ss = "stun:" + stunUser + "@" + host;
		if (w.stunPort->value() != 3478)
			ss += QString(":%1").arg(w.stunPort->value());
		updates["stunserver"] = ss;
		if (!stunPass.isEmpty())
			updates["stunpass"] = stunPass;
	}
	updates["answermode"] = answermodeToString(
		w.answermode->currentData().toInt());
	if (w.mediaenc->currentText() != "none")
		updates["mediaenc"] = w.mediaenc->currentText();
	if (w.medianat->currentText() != "none")
		updates["medianat"] = w.medianat->currentText();
	if (!audioCodecs.isEmpty())
		updates["audio_codecs"] = audioCodecs;

	updateAccountsParams(updates, index);

	/* The SIP domain lives inside the <sip:user@domain> AOR, not in
	 * a ;param — update it separately. */
	if (!sipDomain.isEmpty())
		updateAccountsDomain(sipDomain, index);

	/* Apply in-process: nothing to do when the line is unchanged. */
	QString newLine = accountLine(index);
	if (newLine == w.origLine)
		return;

	/* Destroy and recreate the UA from the new line — a fresh reg
	 * client avoids the "unregistering" stall that ua_register()
	 * on a live UA can hit. The mqueue is FIFO, so the free lands
	 * before the alloc. Match the live UA by the *original* AOR
	 * (the file line may already have a new domain). */
	struct ua *ua = findUaByUserHost(w.origLine);
	if (ua)
		qt_mod_ua_free(ua);
	if (w.enabled->isChecked() && !newLine.isEmpty())
		qt_mod_ua_alloc(newLine);

	w.origLine = newLine;
}


void SettingsDialog::saveSettings()
{
	int nAccounts = tabs_->count() - 1; /* exclude Audio tab */
	for (int i = 0; i < nAccounts; ++i)
		saveAccount(accounts_[i], i);

	/* Audio device selection is not persisted yet — baresip writes
	 * the module config itself. */
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


void SettingsDialog::onAddAccount()
{
	int n = tabs_->count() - 1; /* exclude Audio tab */
	if (n >= kMaxAccounts)
		return;

	/* Build the page first so the AccountWidgets fields exist. */
	AccountWidgets &dst = accounts_[n];
	tabs_->insertTab(n, buildAccountPage(dst, tabs_),
			 QString("Account %1").arg(n + 1));

	/* Duplicate the last account's loaded values into the new tab. */
	if (n > 0) {
		AccountWidgets &src = accounts_[n - 1];
		dst.enabled->setChecked(src.enabled->isChecked());
		dst.regint->setValue(src.regint->value());
		dst.stunHost->setText(src.stunHost->text());
		dst.stunPort->setValue(src.stunPort->value());
		dst.stunUser->setText(src.stunUser->text());
		dst.stunPass->setText(src.stunPass->text());
		dst.answermode->setCurrentIndex(src.answermode->currentIndex());
		dst.mediaenc->setCurrentIndex(src.mediaenc->currentIndex());
		dst.medianat->setCurrentIndex(src.medianat->currentIndex());
		dst.audioCodecs->setText(src.audioCodecs->text());
		dst.origLine = src.origLine;
	} else {
		dst.enabled->setChecked(true);
		dst.regint->setValue(600);
		dst.stunPort->setValue(3478);
		dst.answermode->setCurrentIndex(0);
		dst.mediaenc->setCurrentIndex(0);
		dst.medianat->setCurrentIndex(0);
		dst.origLine.clear();
	}

	/* Blank the identity fields and guide the SIP domain. */
	dst.displayName->clear();
	dst.authUser->clear();
	dst.authPass->clear();
	dst.sipDomain->setText("[sipdomain]:[port]");

	tabs_->setCurrentIndex(n);
	updateAddTabVisibility();
}


void SettingsDialog::updateAddTabVisibility()
{
	int n = tabs_->count() - 1; /* exclude Audio tab */
	addTabBtn_->setVisible(n < kMaxAccounts);
	updateRemoveTabVisibility();
}


void SettingsDialog::updateRemoveTabVisibility()
{
	/* The "-" button is shown only when an account tab other than
	 * the first (index >= 1) is the current tab. The Audio tab is
	 * always the last tab, so any current index in [1, nAccounts-1]
	 * qualifies. */
	int idx = tabs_->currentIndex();
	int nAccounts = tabs_->count() - 1; /* exclude Audio tab */
	bool removable = (idx >= 1) && (idx < nAccounts);
	removeTabBtn_->setVisible(removable);
}


void SettingsDialog::onRemoveAccount()
{
	int idx = tabs_->currentIndex();
	int nAccounts = tabs_->count() - 1; /* exclude Audio tab */
	if (idx < 1 || idx >= nAccounts)
		return;

	/* Build an overlay confirmation on the settings panel
	 * instead of a separate modal dialog. The overlay is a
	 * semi-transparent QFrame centered over the panel. */
	if (confirmOverlay_)
		confirmOverlay_->deleteLater();

	confirmOverlay_ = new QFrame(panel_);
	{
		/* Same dialog style as the parent panel — themed
		 * palette(window) fill at ~78% opacity so it still
		 * reads as an overlay, mid-grey border. */
		QColor bg = palette().color(QPalette::Window);
		confirmOverlay_->setStyleSheet(QString(
			"QFrame { background-color: rgba(%1,%2,%3,200);"
			"         border: 2px solid #808080;"
			"         border-radius: 8px; }"
			"QLabel { border: none; color: palette(text); }")
			.arg(bg.red()).arg(bg.green()).arg(bg.blue()));
	}
	confirmOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents,
				      false);

	/* 10% horizontal, 30% vertical margins so the message
	 * renders on one line and the overlay stays inset. */
	int mw = panel_->width() / 10;
	int mh = panel_->height() * 3 / 10;
	confirmOverlay_->setGeometry(mw, mh,
		panel_->width() - 2 * mw,
		panel_->height() - 2 * mh);

	auto *olay = new QVBoxLayout(confirmOverlay_);
	olay->addStretch();

	auto *msg = new QLabel(
		QString("Remove account %1? This deletes the account "
			"record from the accounts file.")
			.arg(idx + 1), confirmOverlay_);
	msg->setStyleSheet("border: none; color: palette(text);"
			   " font-size: 14px; font-weight: bold;"
			   " padding-bottom: 1.0em;");
	msg->setAlignment(Qt::AlignCenter);
	msg->setWordWrap(true);
	olay->addWidget(msg);

	auto *btnRow = new QHBoxLayout();
	btnRow->setAlignment(Qt::AlignCenter);
	btnRow->setSpacing(20);

	auto *yesBtn = new QPushButton("Yes", confirmOverlay_);
	yesBtn->setStyleSheet(
		"QPushButton { background-color: #d32f2f; color: white;"
		"             border: none; border-radius: 4px;"
		"             padding: 6px 20px; font-weight: bold; }"
		"QPushButton:hover { background-color: #b71c1c; }");
	auto *noBtn = new QPushButton("No", confirmOverlay_);
	noBtn->setStyleSheet(
		"QPushButton { background-color: #424242; color: white;"
		"             border: none; border-radius: 4px;"
		"             padding: 6px 20px; font-weight: bold; }"
		"QPushButton:hover { background-color: #616161; }");

	btnRow->addWidget(yesBtn);
	btnRow->addWidget(noBtn);
	olay->addLayout(btnRow);
	olay->addStretch();

	/* Size the overlay to cover the panel. */
	confirmOverlay_->show();
	confirmOverlay_->raise();

	/* Yes — proceed with removal. */
	connect(yesBtn, &QPushButton::clicked, this, [this, idx]() {
		if (confirmOverlay_)
			confirmOverlay_->deleteLater();
		confirmOverlay_ = nullptr;

		int n = tabs_->count() - 1; /* exclude Audio tab */

		/* Destroy the live UA for this account (if any). */
		struct ua *ua = findUaByUserHost(
			accounts_[idx].origLine);
		if (ua)
			qt_mod_ua_free(ua);

		/* Remove the line from ~/.baresip/accounts. */
		removeAccountLine(idx);

		/* Remove the tab. */
		tabs_->removeTab(idx);

		/* Shift the AccountWidgets entries down so slot i
		 * holds the data for the (now renumbered) tab i. */
		for (int i = idx; i < n - 1; ++i)
			accounts_[i] = accounts_[i + 1];

		/* Clear the now-vacant top slot. */
		accounts_[n - 1] = AccountWidgets{};

		/* Renumber the remaining account tabs. */
		for (int i = 0; i < n - 1; ++i)
			tabs_->setTabText(i,
				QString("Account %1").arg(i + 1));

		updateAddTabVisibility();
	});

	/* No — cancel. */
	connect(noBtn, &QPushButton::clicked, this, [this]() {
		if (confirmOverlay_)
			confirmOverlay_->deleteLater();
		confirmOverlay_ = nullptr;
	});
}
