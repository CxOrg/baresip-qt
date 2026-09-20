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

class QMenu;
class QAction;

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


/* ---- DBusMenu server -------------------------------------------- */

class DBusMenu : public QObject {
	Q_OBJECT
	Q_CLASSINFO("D-Bus Interface", "com.canonical.dbusmenu")

	Q_PROPERTY(uint Version READ version CONSTANT)
	Q_PROPERTY(QString TextDirection READ textDirection CONSTANT)
	Q_PROPERTY(QString Status READ status CONSTANT)
	Q_PROPERTY(QStringList IconThemePath READ iconThemePath CONSTANT)

public:
	explicit DBusMenu(QMenu *menu, QObject *parent = nullptr);

	uint version() const { return 4; }
	QString textDirection() const { return "LTR"; }
	QString status() const { return "normal"; }
	QStringList iconThemePath() const { return {}; }

	/* Assign a stable ID to each action in the menu tree. */
	int idForAction(QAction *act) const;
	QAction *actionForId(int id) const;

public slots:
	/* D-Bus methods (com.canonical.dbusmenu) */
	uint GetLayout(int parentId, int recursionDepth,
		       const QStringList &propertyNames,
		       DBusMenuLayoutItem &layout);
	QList<DBusMenuItem> GetGroupProperties(const QList<int> &ids,
					       const QStringList &propertyNames);
	QVariant GetProperty(int id, const QString &name);
	void Event(int id, const QString &eventId, const QVariant &data,
		   uint timestamp);
	QList<int> EventGroup(const QList<int> &ids, const QString &eventId,
			      const QList<QVariant> &data, uint timestamp);
	bool AboutToShow(int id);
	QList<int> AboutToShowGroup(const QList<int> &ids,
				    QList<bool> &needUpdates);

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


/* ---- StatusNotifierItem ------------------------------------------ */

class StatusNotifierItem : public QObject {
	Q_OBJECT
	Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")

	Q_PROPERTY(QString Id READ id CONSTANT)
	Q_PROPERTY(QString Title READ title NOTIFY titleChanged)
	Q_PROPERTY(QString Status READ status NOTIFY statusChanged)
	Q_PROPERTY(QString Category READ category CONSTANT)
	Q_PROPERTY(QString IconName READ iconName NOTIFY iconChanged)
	Q_PROPERTY(QString IconThemePath READ iconThemePath CONSTANT)
	Q_PROPERTY(QString AttentionIconName READ attentionIconName NOTIFY iconChanged)
	Q_PROPERTY(QString IconAccessibleDesc READ iconAccessibleDesc CONSTANT)
	Q_PROPERTY(QString AttentionAccessibleDesc READ attentionAccessibleDesc CONSTANT)
	Q_PROPERTY(QDBusObjectPath Menu READ menuPath CONSTANT)

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
	QString attentionIconName() const { return m_attentionIconName; }
	QString iconAccessibleDesc() const { return "baresip"; }
	QString attentionAccessibleDesc() const { return "baresip attention"; }
	QDBusObjectPath menuPath() const { return QDBusObjectPath("/MenuBar"); }

signals:
	/* Emitted when Plasma calls Activate(x, y) -- left-click.
	 * x, y are the icon's screen coordinates. */
	void activated(int x, int y);
	/* Emitted when Plasma calls ContextMenu(x, y) -- right-click. */
	void contextMenuRequested(int x, int y);

	/* SNI property-change signals */
	void titleChanged();
	void statusChanged(const QString &status);
	void iconChanged();

	/* SNI notification signals */
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
