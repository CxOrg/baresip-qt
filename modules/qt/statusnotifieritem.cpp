/**
 * @file qt/statusnotifieritem.cpp Native org.kde.StatusNotifierItem D-Bus tray
 */
#include "statusnotifieritem.h"

#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QScreen>
#include <QTemporaryFile>
#include <QDir>
#include <QDebug>

/* Register the custom DBusMenu types with the D-Bus marshalling system. */
static void registerMetaTypes()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;

	qDBusRegisterMetaType<DBusMenuItem>();
	qDBusRegisterMetaType<DBusMenuItemKeys>();
	qDBusRegisterMetaType<DBusMenuLayoutItem>();
	qDBusRegisterMetaType<QList<DBusMenuItem>>();
	qDBusRegisterMetaType<QList<DBusMenuItemKeys>>();
	qDBusRegisterMetaType<QList<DBusMenuLayoutItem>>();
}


/* ---- DBusMenu ----------------------------------------------------- */

DBusMenu::DBusMenu(QMenu *menu, QObject *parent)
	: QObject(parent), menu_(menu)
{
	registerMetaTypes();
	rebuildIdMap();

	/* When the menu structure changes, rebuild the ID map and
	 * notify Plasma that the layout changed. */
	connect(menu_, &QMenu::aboutToShow, this, [this]() {
		rebuildIdMap();
		emit LayoutUpdated(++revision_, 0);
	});
}


void DBusMenu::rebuildIdMap()
{
	actionIds_.clear();
	idActions_.clear();
	nextId_ = 1;
	mapActions(menu_);
}


void DBusMenu::mapActions(QMenu *menu)
{
	for (QAction *act : menu->actions()) {
		if (act->isSeparator())
			continue;
		int id = nextId_++;
		actionIds_.insert(act, id);
		idActions_.insert(id, act);
		if (act->menu())
			mapActions(act->menu());
	}
}


int DBusMenu::idForAction(QAction *act) const
{
	return actionIds_.value(act, -1);
}


QAction *DBusMenu::actionForId(int id) const
{
	return idActions_.value(id, nullptr);
}


QVariantMap DBusMenu::propsForAction(QAction *act,
				     const QStringList &propertyNames) const
{
	QVariantMap props;

	/* If propertyNames is empty, return all properties. */
	bool all = propertyNames.isEmpty();

	auto want = [&](const QString &name) {
		return all || propertyNames.contains(name);
	};

	if (act->isSeparator()) {
		if (want("type"))
			props["type"] = "separator";
		return props;
	}

	if (want("label"))
		props["label"] = act->text().replace("&", "");
	if (want("enabled"))
		props["enabled"] = act->isEnabled();
	if (want("visible"))
		props["visible"] = act->isVisible();
	if (want("type")) {
		if (act->isCheckable())
			props["type"] = act->actionGroup() &&
				act->actionGroup()->isExclusive()
				? "radio" : "checkbox";
		else
			props["type"] = "standard";
	}
	if (want("toggle-state") && act->isCheckable())
		props["toggle-state"] = act->isChecked() ? 1 : 0;
	if (want("icon-name")) {
		QIcon icon = act->icon();
		if (!icon.isNull())
			props["icon-name"] = act->icon().name();
	}
	if (want("children-display") && act->menu())
		props["children-display"] = "submenu";

	return props;
}


void DBusMenu::populateLayoutItem(DBusMenuLayoutItem &item, QAction *act,
				  int depth,
				  const QStringList &propertyNames) const
{
	item.id = idForAction(act);
	item.properties = propsForAction(act, propertyNames);

	if (depth > 0 && act->menu()) {
		for (QAction *child : act->menu()->actions()) {
			if (child->isSeparator() && depth == 1)
				continue;
			DBusMenuLayoutItem childItem;
			populateLayoutItem(childItem, child, depth - 1,
					   propertyNames);
			item.children.append(
				QVariant::fromValue(childItem));
		}
	}
}


uint DBusMenu::GetLayout(int parentId, int recursionDepth,
			 const QStringList &propertyNames,
			 DBusMenuLayoutItem &layout)
{
	layout.id = parentId;
	layout.properties.clear();
	layout.children.clear();

	if (parentId == 0) {
		/* Root: return the menu's top-level actions. */
		if (recursionDepth == 0)
			recursionDepth = -1;  /* include children */
		for (QAction *act : menu_->actions()) {
			DBusMenuLayoutItem item;
			populateLayoutItem(item, act, recursionDepth - 1,
					   propertyNames);
			layout.children.append(
				QVariant::fromValue(item));
		}
	} else {
		QAction *act = actionForId(parentId);
		if (act && act->menu()) {
			for (QAction *child : act->menu()->actions()) {
				DBusMenuLayoutItem item;
				populateLayoutItem(item, child,
						   recursionDepth - 1,
						   propertyNames);
				layout.children.append(
					QVariant::fromValue(item));
			}
		}
	}

	return revision_;
}


QList<DBusMenuItem> DBusMenu::GetGroupProperties(const QList<int> &ids,
						  const QStringList &propertyNames)
{
	QList<DBusMenuItem> result;
	for (int id : ids) {
		QAction *act = actionForId(id);
		if (!act)
			continue;
		DBusMenuItem item;
		item.id = id;
		item.properties = propsForAction(act, propertyNames);
		result.append(item);
	}
	return result;
}


QVariant DBusMenu::GetProperty(int id, const QString &name)
{
	QAction *act = actionForId(id);
	if (!act)
		return QVariant();
	return propsForAction(act, {name}).value(name);
}


void DBusMenu::Event(int id, const QString &eventId, const QVariant &data,
		     uint timestamp)
{
	Q_UNUSED(data)
	Q_UNUSED(timestamp)

	if (eventId == "clicked") {
		QAction *act = actionForId(id);
		if (act && act->isEnabled()) {
			emit itemActivated(id);
			act->trigger();
		}
	}
}


QList<int> DBusMenu::EventGroup(const QList<int> &ids,
				const QString &eventId,
				const QList<QVariant> &data,
				uint timestamp)
{
	QList<int> errors;
	for (int i = 0; i < ids.size(); ++i) {
		int id = ids[i];
		QVariant d = (i < data.size()) ? data[i] : QVariant();
		if (eventId == "clicked") {
			QAction *act = actionForId(id);
			if (!act) {
				errors.append(id);
			} else if (act->isEnabled()) {
				emit itemActivated(id);
				act->trigger();
			}
		} else {
			Event(id, eventId, d, timestamp);
		}
	}
	return errors;
}


bool DBusMenu::AboutToShow(int id)
{
	Q_UNUSED(id)
	rebuildIdMap();
	return false;
}


QList<int> DBusMenu::AboutToShowGroup(const QList<int> &ids,
				      QList<bool> &needUpdates)
{
	QList<int> errors;
	for (int id : ids) {
		needUpdates.append(false);
	}
	return errors;
}


/* ---- StatusNotifierItem ------------------------------------------ */

StatusNotifierItem::StatusNotifierItem(QObject *parent)
	: QObject(parent)
{
	registerMetaTypes();

	/* Unique ID: app name + PID. */
	m_id = QString("baresip-%1").arg(QCoreApplication::applicationPid());
	m_title = "baresip";
	m_iconName = "call-start";
}


StatusNotifierItem::~StatusNotifierItem()
{
	if (m_registered) {
		QDBusConnection::sessionBus().unregisterObject(
			"/StatusNotifierItem");
		QDBusConnection::sessionBus().unregisterObject("/MenuBar");
	}
}


bool StatusNotifierItem::registerOnBus()
{
	auto bus = QDBusConnection::sessionBus();

	/* Register a unique service name. D-Bus well-known names must
	 * match [a-zA-Z_-][a-zA-Z0-9_-]* per segment, so use the PID
	 * (decimal) plus a small counter to guarantee uniqueness. */
	static int counter = 0;
	QString serviceName = QString("org.kde.StatusNotifierItem-%1-%2")
		.arg(QCoreApplication::applicationPid())
		.arg(counter++);

	if (!bus.registerService(serviceName)) {
		qWarning() << "SNI: failed to register service" << serviceName
			   << ":" << bus.lastError().message();
		return false;
	}

	if (!bus.registerObject("/StatusNotifierItem", this)) {
		qWarning() << "SNI: failed to register object:"
			   << bus.lastError().message();
		return false;
	}

	/* Register the DBusMenu object at /MenuBar. */
	if (m_dbusMenu) {
		if (!bus.registerObject("/MenuBar", m_dbusMenu)) {
			qWarning() << "SNI: failed to register menu object:"
				   << bus.lastError().message();
		}
	}

	/* Register with the StatusNotifierWatcher. */
	QDBusMessage msg = QDBusMessage::createMethodCall(
		"org.kde.StatusNotifierWatcher",
		"/StatusNotifierWatcher",
		"org.kde.StatusNotifierWatcher",
		"RegisterStatusNotifierItem");
	msg << serviceName;

	bus.call(msg);  /* synchronous: the watcher must know us before show */

	m_registered = true;
	return true;
}


void StatusNotifierItem::setMenu(QMenu *menu)
{
	if (m_dbusMenu)
		delete m_dbusMenu;
	m_dbusMenu = new DBusMenu(menu, this);

	if (m_registered) {
		QDBusConnection::sessionBus().registerObject(
			"/MenuBar", m_dbusMenu);
	}
}


void StatusNotifierItem::setIcon(const QIcon &icon)
{
	m_icon = icon;
	/* Try to use a theme name; fall back to writing a temp file. */
	QString name = icon.name();
	if (!name.isEmpty())
		m_iconName = name;
	emit iconChanged();
	emit NewIcon();
}


void StatusNotifierItem::setIconName(const QString &name)
{
	m_iconName = name;
	m_icon = QIcon::fromTheme(name);
	emit iconChanged();
	emit NewIcon();
}


void StatusNotifierItem::setTitle(const QString &title)
{
	m_title = title;
	emit titleChanged();
	emit NewTitle();
}


void StatusNotifierItem::setStatus(const QString &status)
{
	m_status = status;
	emit statusChanged(status);
	emit NewStatus(status);
}


void StatusNotifierItem::show()
{
	if (!m_registered)
		registerOnBus();
}


void StatusNotifierItem::ContextMenu(int x, int y)
{
	emit contextMenuRequested(x, y);
}


void StatusNotifierItem::Activate(int x, int y)
{
	emit activated(x, y);
}


void StatusNotifierItem::SecondaryActivate(int x, int y)
{
	Q_UNUSED(x)
	Q_UNUSED(y)
}


void StatusNotifierItem::Scroll(int delta, const QString &orientation)
{
	Q_UNUSED(delta)
	Q_UNUSED(orientation)
}
