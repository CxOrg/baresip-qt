/**
 * @file qt/call_history.h Qt UI module -- persistent call history
 *
 * Stores call log entries to ~/.baresip/call_history.csv so they
 * survive restarts. Each entry: timestamp,type,number,info,duration,uri
 *   type: 0=incoming, 1=outgoing, 2=missed, 3=rejected
 *   number: dialable phone number, empty for SIP-address peers
 *   uri: full "sip:user@host" peer URI (column added later — absent
 *        in files written by older versions)
 */
#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QDateTime>

struct CallHistoryEntry {
	QDateTime ts;
	int       type;   /* CALL_INCOMING / CALL_OUTGOING / CALL_MISSED / CALL_REJECTED */
	QString   number; /* dialable phone number, empty for SIP peers */
	QString   uri;    /* full "sip:user@host" peer URI, may be empty */
	QString   info;   /* peer display name, may be empty */
	uint32_t  duration = 0;  /* call duration in seconds (0 if not connected) */
	int       count = 1;     /* number of calls to this peer (in/out) */

	/** What the user sees/dials: the number if present, else URI. */
	QString target() const { return number.isEmpty() ? uri : number; }
};

class CallHistory : public QObject {
	Q_OBJECT

public:
	static CallHistory *instance();

	/** Append an entry and persist to disk. */
	void add(const QString &number, const QString &uri, int type,
		 const QString &info);

	/** Update the duration of the most recent entry matching `uri`
	 *  (falls back to matching the number column). */
	void updateDuration(const QString &uri, uint32_t duration);

	/** Remove the entry matching ts+number+uri; saves and emits
	 *  changed(). Returns false if no entry matched. */
	bool remove(const QDateTime &ts, const QString &number,
		    const QString &uri);

	/** Load the most recent `n` entries from disk. */
	QList<CallHistoryEntry> recent(int n) const;

	/** Load all entries from disk (replaces in-memory list). */
	void load();

	/** Path to the CSV file (~/ .baresip / call_history.csv). */
	static QString filePath();

signals:
	/** Emitted after a new entry is added and persisted. */
	void changed();

private:
	CallHistory(QObject *parent = nullptr);
	QList<CallHistoryEntry> entries_;
	void save() const;
};
