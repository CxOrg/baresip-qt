/**
 * @file qt/statusnotifieritem.h Native org.kde.StatusNotifierItem D-Bus tray
 *
 * Replaces QSystemTrayIcon with a direct D-Bus implementation of the
 * KDE StatusNotifierItem protocol. The key advantage over QSystemTrayIcon
 * is that Plasma passes the icon's screen coordinates (x, y) to
 * Activate() and ContextMenu(), which QSystemTrayIcon discards.
 *
 * Also implements a minimal com.canonical.dbusmenu server so Plasma
 * can render the right-click context menu natively (as a Wayland
 * surface) rather than requiring a Qt popup grab.
 */
#pragma once

#include <QObject>
#include <QString>
#include <QIcon>
#include <QHash>
#include <QDBusObjectPath>
#include <QDBusArgument>
#include <QDBusAbstractAdaptor>
#include <QVariantMap>

class QMenu;
class QAction;
class StatusNotifierItem;
class DBusMenu;

/* ---- DBusMenu layout types (com.canonical.dbusmenu) -------------- */

struct DBusMenuItem {
	int id = 0;
	QVariantMap properties;
};
Q_DECLARE_METATYPE(DBusMenuItem)

struct DBusMenuItemKeys {
	int id = 0;
	QStringList properties;
};
Q_DECLARE_METATYPE(DBusMenuItemKeys)

struct DBusMenuLayoutItem {
	int id = 0;
	QVariantMap properties;
	QList<QVariant> children;  /* each is QDBusVariant wrapping DBusMenuLayoutItem */
};
Q_DECLARE_METATYPE(DBusMenuLayoutItem)

/* QDBusArgument streaming operators (required by qDBusRegisterMetaType) */
inline QDBusArgument &operator<<(QDBusArgument &arg, const DBusMenuItem &item)
{
	arg.beginStructure();
	arg << item.id << item.properties;
	arg.endStructure();
	return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, DBusMenuItem &item)
{
	arg.beginStructure();
	arg >> item.id >> item.properties;
	arg.endStructure();
	return arg;
}

inline QDBusArgument &operator<<(QDBusArgument &arg, const DBusMenuItemKeys &keys)
{
	arg.beginStructure();
	arg << keys.id << keys.properties;
	arg.endStructure();
	return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, DBusMenuItemKeys &keys)
{
	arg.beginStructure();
	arg >> keys.id >> keys.properties;
	arg.endStructure();
	return arg;
}

inline QDBusArgument &operator<<(QDBusArgument &arg, const DBusMenuLayoutItem &item)
{
	arg.beginStructure();
	arg << item.id << item.properties << item.children;
	arg.endStructure();
	return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, DBusMenuLayoutItem &item)
{
	arg.beginStructure();
	arg >> item.id >> item.properties >> item.children;
	arg.endStructure();
	return arg;
}


/* ---- SNI IconPixmap type (a(iiay)) ------------------------------- */

struct IconPixmap {
	int width = 0;
	int height = 0;
	QByteArray data;  /* ARGB32, network byte order, row-major */
};
Q_DECLARE_METATYPE(IconPixmap)

inline QDBusArgument &operator<<(QDBusArgument &arg, const IconPixmap &pix)
{
	arg.beginStructure();
	arg << pix.width << pix.height << pix.data;
	arg.endStructure();
	return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, IconPixmap &pix)
{
	arg.beginStructure();
	arg >> pix.width >> pix.height >> pix.data;
	arg.endStructure();
	return arg;
}

typedef QList<IconPixmap> IconPixmapList;
Q_DECLARE_METATYPE(IconPixmapList)


/* ---- DBusMenu server -------------------------------------------- */

class DBusMenu : public QObject {
	Q_OBJECT

public:
	explicit DBusMenu(QMenu *menu, QObject *parent = nullptr);

	uint version() const { return 4; }
	QString textDirection() const { return "LTR"; }
	QString status() const { return "normal"; }
	QStringList iconThemePath() const { return {}; }

	/* Assign a stable ID to each action in the menu tree. */
	int idForAction(QAction *act) const;
	QAction *actionForId(int id) const;

	/* D-Bus methods (com.canonical.dbusmenu) -- called by adaptor */
	uint getLayout(int parentId, int recursionDepth,
		       const QStringList &propertyNames,
		       DBusMenuLayoutItem &layout);
	QList<DBusMenuItem> getGroupProperties(const QList<int> &ids,
					       const QStringList &propertyNames);
	QVariant getProperty(int id, const QString &name);
	void event(int id, const QString &eventId, const QVariant &data,
		   uint timestamp);
	QList<int> eventGroup(const QList<int> &ids, const QString &eventId,
			      const QList<QVariant> &data, uint timestamp);
	bool aboutToShow(int id);
	QList<int> aboutToShowGroup(const QList<int> &ids,
				    QList<bool> &needUpdates);

	void emitLayoutUpdated();

signals:
	void ItemsPropertiesUpdated(
		const QList<DBusMenuItem> &updated,
		const QList<DBusMenuItem> &removed);
	void LayoutUpdated(uint revision, int parent);
	void itemActivated(int id);

private:
	void rebuildIdMap();
	void mapActions(QMenu *menu);
	QVariantMap propsForAction(QAction *act,
				   const QStringList &propertyNames) const;
	void populateLayoutItem(DBusMenuLayoutItem &item, QAction *act,
				int depth, const QStringList &propertyNames) const;

	QMenu *menu_ = nullptr;
	QHash<QAction *, int> actionIds_;
	QHash<int, QAction *> idActions_;
	int nextId_ = 1;  /* 0 = root menu */
	uint revision_ = 1;
};


/* ---- DBusMenu Adaptor -------------------------------------------- */

class DBusMenuAdaptor : public QDBusAbstractAdaptor {
	Q_OBJECT
	Q_CLASSINFO("D-Bus Interface", "com.canonical.dbusmenu")
	Q_CLASSINFO("D-Bus Introspection", ""
"  <interface name=\"com.canonical.dbusmenu\">\n"
"    <property access=\"read\" type=\"u\" name=\"Version\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"TextDirection\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"Status\"/>\n"
"    <property access=\"read\" type=\"as\" name=\"IconThemePath\"/>\n"
"    <method name=\"GetLayout\">\n"
"      <arg direction=\"in\" type=\"i\" name=\"parentId\"/>\n"
"      <arg direction=\"in\" type=\"i\" name=\"recursionDepth\"/>\n"
"      <arg direction=\"in\" type=\"as\" name=\"propertyNames\"/>\n"
"      <arg direction=\"out\" type=\"u\" name=\"revision\"/>\n"
"      <arg direction=\"out\" type=\"(ia{sv}av)\" name=\"layout\"/>\n"
"    </method>\n"
"    <method name=\"GetGroupProperties\">\n"
"      <arg direction=\"in\" type=\"ai\" name=\"ids\"/>\n"
"      <arg direction=\"in\" type=\"as\" name=\"propertyNames\"/>\n"
"      <arg direction=\"out\" type=\"a(ia{sv})\" name=\"properties\"/>\n"
"    </method>\n"
"    <method name=\"GetProperty\">\n"
"      <arg direction=\"in\" type=\"i\" name=\"id\"/>\n"
"      <arg direction=\"in\" type=\"s\" name=\"name\"/>\n"
"      <arg direction=\"out\" type=\"v\" name=\"value\"/>\n"
"    </method>\n"
"    <method name=\"Event\">\n"
"      <arg direction=\"in\" type=\"i\" name=\"id\"/>\n"
"      <arg direction=\"in\" type=\"s\" name=\"eventId\"/>\n"
"      <arg direction=\"in\" type=\"v\" name=\"data\"/>\n"
"      <arg direction=\"in\" type=\"u\" name=\"timestamp\"/>\n"
"    </method>\n"
"    <method name=\"EventGroup\">\n"
"      <arg direction=\"in\" type=\"ai\" name=\"ids\"/>\n"
"      <arg direction=\"in\" type=\"s\" name=\"eventId\"/>\n"
"      <arg direction=\"in\" type=\"av\" name=\"data\"/>\n"
"      <arg direction=\"in\" type=\"u\" name=\"timestamp\"/>\n"
"      <arg direction=\"out\" type=\"ai\" name=\"errors\"/>\n"
"    </method>\n"
"    <method name=\"AboutToShow\">\n"
"      <arg direction=\"in\" type=\"i\" name=\"id\"/>\n"
"      <arg direction=\"out\" type=\"b\" name=\"needUpdate\"/>\n"
"    </method>\n"
"    <method name=\"AboutToShowGroup\">\n"
"      <arg direction=\"in\" type=\"ai\" name=\"ids\"/>\n"
"      <arg direction=\"out\" type=\"ai\" name=\"idErrors\"/>\n"
"      <arg direction=\"out\" type=\"ab\" name=\"updatesNeeded\"/>\n"
"    </method>\n"
"    <signal name=\"ItemsPropertiesUpdated\">\n"
"      <arg direction=\"out\" type=\"a(ia{sv})\" name=\"updated\"/>\n"
"      <arg direction=\"out\" type=\"a(ias)\" name=\"removed\"/>\n"
"    </signal>\n"
"    <signal name=\"LayoutUpdated\">\n"
"      <arg direction=\"out\" type=\"u\" name=\"revision\"/>\n"
"      <arg direction=\"out\" type=\"i\" name=\"parent\"/>\n"
"    </signal>\n"
"  </interface>\n"
"")

public:
	explicit DBusMenuAdaptor(DBusMenu *menu) : QDBusAbstractAdaptor(menu), m_menu(menu) {}

	uint Version() const { return m_menu->version(); }
	QString TextDirection() const { return m_menu->textDirection(); }
	QString Status() const { return m_menu->status(); }
	QStringList IconThemePath() const { return m_menu->iconThemePath(); }

public slots:
	uint GetLayout(int parentId, int recursionDepth,
		       const QStringList &propertyNames,
		       DBusMenuLayoutItem &layout) {
		return m_menu->getLayout(parentId, recursionDepth,
					 propertyNames, layout);
	}
	QList<DBusMenuItem> GetGroupProperties(const QList<int> &ids,
					      const QStringList &propertyNames) {
		return m_menu->getGroupProperties(ids, propertyNames);
	}
	QVariant GetProperty(int id, const QString &name) {
		return m_menu->getProperty(id, name);
	}
	void Event(int id, const QString &eventId, const QVariant &data,
		   uint timestamp) {
		m_menu->event(id, eventId, data, timestamp);
	}
	QList<int> EventGroup(const QList<int> &ids, const QString &eventId,
			      const QList<QVariant> &data, uint timestamp) {
		return m_menu->eventGroup(ids, eventId, data, timestamp);
	}
	bool AboutToShow(int id) { return m_menu->aboutToShow(id); }
	QList<int> AboutToShowGroup(const QList<int> &ids,
				    QList<bool> &needUpdates) {
		return m_menu->aboutToShowGroup(ids, needUpdates);
	}

signals:
	void ItemsPropertiesUpdated(
		const QList<DBusMenuItem> &updated,
		const QList<DBusMenuItem> &removed);
	void LayoutUpdated(uint revision, int parent);

private:
	DBusMenu *m_menu;
};


/* ---- StatusNotifierItem ------------------------------------------ */

class StatusNotifierItem : public QObject {
	Q_OBJECT

public:
	explicit StatusNotifierItem(QObject *parent = nullptr);
	~StatusNotifierItem() override;

	/* Register on the session bus and with the StatusNotifierWatcher. */
	bool registerOnBus();

	/* Set the context menu (exported via DBusMenu). */
	void setMenu(QMenu *menu);

	/* Update icon / title / status. */
	void setIcon(const QIcon &icon);
	void setIconName(const QString &name);
	void setTitle(const QString &title);
	void setStatus(const QString &status);
	void show();

	QString id() const { return m_id; }
	QString title() const { return m_title; }
	QString status() const { return m_status; }
	QString category() const { return "Communications"; }
	QString iconName() const { return m_iconName; }
	QString iconThemePath() const { return {}; }
	IconPixmapList iconPixmap() const;
	IconPixmapList attentionIconPixmap() const { return {}; }
	QString attentionIconName() const { return m_attentionIconName; }
	QString iconAccessibleDesc() const { return "baresip"; }
	QString attentionAccessibleDesc() const { return "baresip attention"; }
	QDBusObjectPath menuPath() const { return QDBusObjectPath("/MenuBar"); }
	QString toolTipString() const { return m_title; }

signals:
	/* Emitted when Plasma calls Activate(x, y) -- left-click.
	 * x, y are the icon's screen coordinates. */
	void activated(int x, int y);
	/* Emitted when Plasma calls ContextMenu(x, y) -- right-click. */
	void contextMenuRequested(int x, int y);

	/* SNI property-change signals (for the adaptor) */
	void titleChanged();
	void statusChanged(const QString &status);
	void iconChanged();

	/* SNI notification signals (for the adaptor) */
	void NewIcon();
	void NewAttentionIcon();
	void NewTitle();
	void NewToolTip();
	void NewStatus(const QString &status);

public slots:
	/* D-Bus methods (org.kde.StatusNotifierItem) */
	void ContextMenu(int x, int y);
	void Activate(int x, int y);
	void SecondaryActivate(int x, int y);
	void Scroll(int delta, const QString &orientation);

private:
	QString m_id;
	QString m_title;
	QString m_status = "Active";
	QString m_iconName;
	QString m_attentionIconName;
	QIcon m_icon;
	DBusMenu *m_dbusMenu = nullptr;
	bool m_registered = false;
};


/* ---- StatusNotifierItem Adaptor --------------------------------- */

class StatusNotifierItemAdaptor : public QDBusAbstractAdaptor {
	Q_OBJECT
	Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")
	Q_CLASSINFO("D-Bus Introspection", ""
"  <interface name=\"org.kde.StatusNotifierItem\">\n"
"    <property access=\"read\" type=\"s\" name=\"Id\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"Title\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"Status\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"Category\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"IconName\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"IconThemePath\"/>\n"
"    <property access=\"read\" type=\"a(iiay)\" name=\"IconPixmap\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"AttentionIconName\"/>\n"
"    <property access=\"read\" type=\"a(iiay)\" name=\"AttentionIconPixmap\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"IconAccessibleDesc\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"AttentionAccessibleDesc\"/>\n"
"    <property access=\"read\" type=\"o\" name=\"Menu\"/>\n"
"    <property access=\"read\" type=\"s\" name=\"ToolTip\"/>\n"
"    <method name=\"ContextMenu\">\n"
"      <arg direction=\"in\" type=\"i\" name=\"x\"/>\n"
"      <arg direction=\"in\" type=\"i\" name=\"y\"/>\n"
"    </method>\n"
"    <method name=\"Activate\">\n"
"      <arg direction=\"in\" type=\"i\" name=\"x\"/>\n"
"      <arg direction=\"in\" type=\"i\" name=\"y\"/>\n"
"    </method>\n"
"    <method name=\"SecondaryActivate\">\n"
"      <arg direction=\"in\" type=\"i\" name=\"x\"/>\n"
"      <arg direction=\"in\" type=\"i\" name=\"y\"/>\n"
"    </method>\n"
"    <method name=\"Scroll\">\n"
"      <arg direction=\"in\" type=\"i\" name=\"delta\"/>\n"
"      <arg direction=\"in\" type=\"s\" name=\"orientation\"/>\n"
"    </method>\n"
"    <signal name=\"NewIcon\"/>\n"
"    <signal name=\"NewAttentionIcon\"/>\n"
"    <signal name=\"NewTitle\"/>\n"
"    <signal name=\"NewToolTip\"/>\n"
"    <signal name=\"NewStatus\">\n"
"      <arg direction=\"out\" type=\"s\" name=\"status\"/>\n"
"    </signal>\n"
"  </interface>\n"
"")

public:
	explicit StatusNotifierItemAdaptor(StatusNotifierItem *item)
		: QDBusAbstractAdaptor(item), m_item(item) {}

	QString Id() const { return m_item->id(); }
	QString Title() const { return m_item->title(); }
	QString Status() const { return m_item->status(); }
	QString Category() const { return m_item->category(); }
	QString IconName() const { return m_item->iconName(); }
	QString IconThemePath() const { return m_item->iconThemePath(); }
	IconPixmapList IconPixmap() const { return m_item->iconPixmap(); }
	QString AttentionIconName() const { return m_item->attentionIconName(); }
	IconPixmapList AttentionIconPixmap() const { return m_item->attentionIconPixmap(); }
	QString IconAccessibleDesc() const { return m_item->iconAccessibleDesc(); }
	QString AttentionAccessibleDesc() const { return m_item->attentionAccessibleDesc(); }
	QDBusObjectPath Menu() const { return m_item->menuPath(); }
	QString ToolTip() const { return m_item->toolTipString(); }

public slots:
	void ContextMenu(int x, int y) { m_item->ContextMenu(x, y); }
	void Activate(int x, int y) { m_item->Activate(x, y); }
	void SecondaryActivate(int x, int y) { m_item->SecondaryActivate(x, y); }
	void Scroll(int delta, const QString &orientation) {
		m_item->Scroll(delta, orientation);
	}

signals:
	void NewIcon();
	void NewAttentionIcon();
	void NewTitle();
	void NewToolTip();
	void NewStatus(const QString &status);

private:
	StatusNotifierItem *m_item;
};
