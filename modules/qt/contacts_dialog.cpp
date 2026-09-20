/**
 * @file qt/contacts_dialog.cpp Qt UI module -- contacts add/edit panel
 */
#include "contacts_dialog.h"
#include "call_history.h"
#include "qt_mod.h"

#include <QLineEdit>
#include <QListWidget>
#include <QTabWidget>
#include <QStackedWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QWindow>
#include <algorithm>

#ifdef HAVE_LAYERSHELL
#include <LayerShellQt/Window>
#endif

#include <re.h>
#include <baresip.h>


/* ---- helpers ------------------------------------------------------- */

static QString contactsPath()
{
	return QDir::homePath() + "/.baresip/contacts";
}

/** Parse a contacts-file line into name/uri/params. Handles
 *  `"Name" <sip:user@host>;params`, bare `<sip:...>` and plain
 *  `sip:...` lines. Returns false for comments/blank lines. */
static bool parseContactLine(const QString &line,
			     ContactsDialog::ContactEntry &e)
{
	QString t = line.trimmed();
	if (t.isEmpty() || t.startsWith('#'))
		return false;

	int lt = t.indexOf('<');
	int gt = t.indexOf('>', lt);
	if (lt >= 0 && gt > lt) {
		e.name = t.left(lt).trimmed();
		if (e.name.startsWith('"') && e.name.endsWith('"')
		    && e.name.length() > 1)
			e.name = e.name.mid(1, e.name.length() - 2);
		e.uri    = t.mid(lt + 1, gt - lt - 1);
		e.params = t.mid(gt + 1).trimmed();
	}
	else {
		e.name.clear();
		e.uri = t;
		e.params.clear();
	}
	return true;
}

/** Render a ContactEntry back to a contacts-file line. */
static QString contactLine(const ContactsDialog::ContactEntry &e)
{
	QString line;
	if (!e.name.isEmpty())
		line = QString("\"%1\" ").arg(e.name);
	line += "<" + e.uri + ">";
	if (!e.params.isEmpty())
		line += e.params;
	return line;
}

/** Host part of a sip URI ("sip:u@host:port;p" -> "host:port"). */
static QString uriHost(const QString &uri)
{
	QString s = uri;
	if (s.startsWith("sip:", Qt::CaseInsensitive))
		s = s.mid(4);
	else if (s.startsWith("sips:", Qt::CaseInsensitive))
		s = s.mid(5);
	int at = s.indexOf('@');
	if (at < 0)
		return QString();
	s = s.mid(at + 1);
	int semi = s.indexOf(';');
	if (semi >= 0)
		s = s.left(semi);
	return s;
}

/** Build a full sip: URI for a bare number — prefer keeping the
 *  original host when editing, else complete via the current
 *  account's domain, else just "sip:<number>". */
static QString completeUri(const QString &number, const QString &origHost)
{
	QString host = origHost;
	if (!host.isEmpty())
		return QString("sip:%1@%2").arg(number, host);

	struct ua *ua = qt_current_ua();
	if (ua) {
		char *s = NULL;
		struct pl pl;
		QByteArray utf8 = number.toUtf8();
		pl.p = utf8.constData();
		pl.l = (size_t)utf8.size();
		if (0 == account_uri_complete_strdup(ua_account(ua),
						     &s, &pl)) {
			QString uri = QString::fromUtf8(s);
			mem_deref(s);
			return uri;
		}
	}
	return "sip:" + number;
}


/* ---- dialog -------------------------------------------------------- */

ContactsDialog::ContactsDialog(QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle("baresip Contacts");
	setAttribute(Qt::WA_DeleteOnClose, false);
	/* Same two-layer structure as the call/settings panels:
	 * transparent dialog, inner #contactsPanel carries the themed
	 * fill, border and 8px rounded corners. */
	setAttribute(Qt::WA_TranslucentBackground);
	setStyleSheet(
		"QFrame#contactsPanel {"
		"  background-color: palette(window);"
		"  border: 2px solid palette(dark);"
		"  border-radius: 8px; }");
	buildUi();
	reloadContacts();
	reloadHistory();
	resize(420, 400);
	setupLayerShell();
}


void ContactsDialog::setupLayerShell()
{
#ifdef HAVE_LAYERSHELL
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
	ls->setScope("baresip-contacts");
	ls->setAnchors(LayerShellQt::Window::Anchors(
		LayerShellQt::Window::AnchorTop |
		LayerShellQt::Window::AnchorRight));
	ls->setMargins(QMargins(0, 58, 8, 0));
	ls->setDesiredSize(size());
#endif
}


/** Build one tab page: a stacked widget holding the list (page 0)
 *  and the overlay edit form (page 1). */
static QStackedWidget *buildTabPage(QListWidget **list,
				    QLineEdit **nameEdit, QLineEdit **numEdit,
				    QPushButton **saveBtn, QPushButton **backBtn,
				    QWidget *parent)
{
	auto *stack = new QStackedWidget(parent);

	*list = new QListWidget(stack);
	stack->addWidget(*list);

	auto *formPage = new QWidget(stack);
	auto *form = new QFormLayout(formPage);
	*nameEdit = new QLineEdit(formPage);
	*numEdit  = new QLineEdit(formPage);
	form->addRow("Name:",   *nameEdit);
	form->addRow("Number:", *numEdit);

	auto *btnRow = new QHBoxLayout();
	*saveBtn = new QPushButton("Save Contact", formPage);
	*backBtn = new QPushButton("Back", formPage);
	btnRow->addWidget(*saveBtn);
	btnRow->addStretch();
	btnRow->addWidget(*backBtn);
	form->addRow(btnRow);

	stack->addWidget(formPage);
	return stack;
}


void ContactsDialog::buildUi()
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(2, 2, 2, 2);

	auto *panel = new QFrame(this);
	panel->setObjectName("contactsPanel");
	panel->setAutoFillBackground(true);
	outer->addWidget(panel);

	auto *layout = new QVBoxLayout(panel);
	tabs_ = new QTabWidget(panel);
	layout->addWidget(tabs_);

	/* ---- Contacts tab ---- */
	QPushButton *cSave, *cBack;
	contactsStack_ = buildTabPage(&contactsList_, &cNameEdit_,
				      &cNumEdit_, &cSave, &cBack, tabs_);
	tabs_->addTab(contactsStack_, "Contacts");

	connect(contactsList_, &QListWidget::itemClicked,
		this, &ContactsDialog::onContactClicked);
	connect(cSave, &QPushButton::clicked,
		this, &ContactsDialog::onSaveContactEdit);
	connect(cBack, &QPushButton::clicked,
		this, &ContactsDialog::onCancelContactEdit);

	/* ---- History tab ---- */
	QPushButton *hSave, *hBack;
	historyStack_ = buildTabPage(&historyList_, &hNameEdit_,
				     &hNumEdit_, &hSave, &hBack, tabs_);
	tabs_->addTab(historyStack_, "History");

	connect(historyList_, &QListWidget::itemClicked,
		this, &ContactsDialog::onHistoryClicked);
	connect(hSave, &QPushButton::clicked,
		this, &ContactsDialog::onSaveHistoryContact);
	connect(hBack, &QPushButton::clicked,
		this, &ContactsDialog::onCancelHistoryEdit);

	connect(tabs_, &QTabWidget::currentChanged,
		this, &ContactsDialog::onTabChanged);

	/* ---- Close button ---- */
	auto *btnRow = new QHBoxLayout();
	layout->addLayout(btnRow);
	auto *closeBtn = new QPushButton("Close", panel);
	btnRow->addStretch();
	btnRow->addWidget(closeBtn);
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}


void ContactsDialog::reloadContacts()
{
	entries_.clear();
	preservedLines_.clear();
	contactsList_->clear();

	QFile f(contactsPath());
	if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		while (!f.atEnd()) {
			QString line = QString::fromUtf8(f.readLine());
			ContactEntry e;
			if (parseContactLine(line, e)) {
				QString number = uriToNumber(
					e.uri.toUtf8().constData());
				QString label = e.name.isEmpty()
					? number
					: QString("%1  %2").arg(e.name, number);
				auto *item = new QListWidgetItem(label);
				item->setData(Qt::UserRole, entries_.size());
				contactsList_->addItem(item);
				entries_.append(e);
			}
			else {
				preservedLines_.append(line);
			}
		}
		f.close();
	}
}


void ContactsDialog::reloadHistory()
{
	historyList_->clear();

	/* Unique numbers (most recent first) with a call count. */
	QStringList order;
	QMap<QString, int> counts;
	QMap<QString, QString> names;
	QList<CallHistoryEntry> entries = CallHistory::instance()->recent(200);
	for (int i = entries.size() - 1; i >= 0; --i) {
		const CallHistoryEntry &e = entries[i];
		if (e.uri.isEmpty())
			continue;
		counts[e.uri]++;
		if (!e.info.isEmpty() && !names.contains(e.uri))
			names[e.uri] = e.info;
		if (!order.contains(e.uri))
			order.append(e.uri);
	}

	for (const QString &uri : order) {
		QString label = names.value(uri).isEmpty()
			? uri
			: QString("%1  %2").arg(names.value(uri), uri);
		label += QString("  (%1)").arg(counts.value(uri));
		auto *item = new QListWidgetItem(label);
		item->setData(Qt::UserRole, uri);
		item->setData(Qt::UserRole + 1, names.value(uri));
		historyList_->addItem(item);
	}
}


/** Sort entries by name, write the sorted contacts file and sync
 *  the in-memory contact list on the re thread. */
void ContactsDialog::saveContacts()
{
	std::sort(entries_.begin(), entries_.end(),
		  [](const ContactEntry &a, const ContactEntry &b) {
		int c = a.name.compare(b.name, Qt::CaseInsensitive);
		if (c != 0)
			return c < 0;
		return uriToNumber(a.uri.toUtf8().constData())
			< uriToNumber(b.uri.toUtf8().constData());
	});

	QStringList lines;
	for (const ContactEntry &e : entries_)
		lines << contactLine(e);

	QFile f(contactsPath());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate
		   | QIODevice::Text)) {
		QTextStream out(&f);
		for (const QString &l : preservedLines_)
			out << l;
		for (const QString &l : lines)
			out << l << "\n";
		out.flush();
		f.close();
	}

	/* Rebuild baresip's in-memory contact list (re thread). */
	qt_mod_sync_contacts(lines);

	emit contactsSaved();
}


void ContactsDialog::onTabChanged(int index)
{
	/* Switching to a tab always returns to its list and reloads
	 * it — the Contacts tab re-reads the contacts file. */
	if (index == 0) {
		contactsStack_->setCurrentIndex(0);
		reloadContacts();
	}
	else {
		historyStack_->setCurrentIndex(0);
		reloadHistory();
	}
}


void ContactsDialog::onContactClicked(QListWidgetItem *item)
{
	editingIndex_ = item->data(Qt::UserRole).toInt();
	if (editingIndex_ < 0 || editingIndex_ >= entries_.size())
		return;

	const ContactEntry &e = entries_[editingIndex_];
	cNameEdit_->setText(e.name);
	cNumEdit_->setText(uriToNumber(e.uri.toUtf8().constData()));
	contactsStack_->setCurrentIndex(1);
}


void ContactsDialog::onHistoryClicked(QListWidgetItem *item)
{
	hNumEdit_->setText(item->data(Qt::UserRole).toString());
	hNameEdit_->setText(item->data(Qt::UserRole + 1).toString());
	historyStack_->setCurrentIndex(1);
}


void ContactsDialog::onSaveContactEdit()
{
	QString name   = cNameEdit_->text().trimmed();
	QString number = cNumEdit_->text().trimmed();
	if (number.isEmpty() || editingIndex_ < 0
	    || editingIndex_ >= entries_.size())
		return;

	ContactEntry &e = entries_[editingIndex_];
	QString oldNumber = uriToNumber(e.uri.toUtf8().constData());
	e.name = name;
	if (number != oldNumber)
		e.uri = completeUri(number, uriHost(e.uri));

	saveContacts();
	reloadContacts();
	contactsStack_->setCurrentIndex(0);
}


void ContactsDialog::onSaveHistoryContact()
{
	QString name   = hNameEdit_->text().trimmed();
	QString number = hNumEdit_->text().trimmed();
	if (number.isEmpty())
		return;

	ContactEntry e;
	e.name   = name;
	e.uri    = completeUri(number, QString());
	entries_.append(e);

	saveContacts();
	reloadContacts();
	reloadHistory();
	historyStack_->setCurrentIndex(0);
}


void ContactsDialog::onCancelContactEdit()
{
	contactsStack_->setCurrentIndex(0);
}


void ContactsDialog::onCancelHistoryEdit()
{
	historyStack_->setCurrentIndex(0);
}
