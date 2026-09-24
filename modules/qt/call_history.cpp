/**
 * @file qt/call_history.cpp Qt UI module -- persistent call history
 */
#include "call_history.h"
#include "qt_mod.h"

#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QStandardPaths>


CallHistory *CallHistory::instance()
{
	static CallHistory inst;
	return &inst;
}


CallHistory::CallHistory(QObject *parent)
	: QObject(parent)
{
	load();
}


QString CallHistory::filePath()
{
	/* ~/.baresip/call_history.csv -- co-located with baresip config. */
	QString dir = QStandardPaths::writableLocation(
		QStandardPaths::GenericConfigLocation);
	/* GenericConfigLocation is usually ~/.config on Linux, but baresip
	 * keeps everything in ~/.baresip. Prefer that if HOME is set. */
	QString home = QDir::homePath();
	if (!home.isEmpty())
		dir = home + "/.baresip";

	return dir + "/call_history.csv";
}


void CallHistory::load()
{
	entries_.clear();

	QFile f(filePath());
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		/* First install: create a template with dummy
		 * entries so the history list isn't empty. */
		QDir().mkpath(QFileInfo(filePath()).absolutePath());
		QFile tf(filePath());
		if (tf.open(QIODevice::WriteOnly | QIODevice::Text)) {
			QTextStream out(&tf);
			out << "2026-01-01T10:00:00,0,1234567890,,0,"
			    "sip:1234567890@example.com,1\n"
			    "2026-01-01T11:00:00,1,9876543210,,0,"
			    "sip:9876543210@example.com,1\n"
			    "2026-01-01T12:00:00,0,,sip:mobile@example.com,0,"
			    "sip:mobile@example.com,1\n";
			tf.close();
		}
		return;
	}

	QTextStream in(&f);
	while (!in.atEnd()) {
		QString line = in.readLine().trimmed();
		if (line.isEmpty())
			continue;

		/* CSV: ts,type,uri,info,duration
		 * (info may contain commas -> we split only on the first
		 * 4 commas; the rest is info). */
		QStringList parts;
		int idx = 0;
		for (int i = 0; i < 4; ++i) {
			int comma = line.indexOf(',', idx);
			if (comma < 0) {
				parts.clear();
				break;
			}
			parts << line.mid(idx, comma - idx);
			idx = comma + 1;
		}
		parts << line.mid(idx);

		if (parts.size() < 4)
			continue;

		CallHistoryEntry e;
		e.ts   = QDateTime::fromString(parts[0], Qt::ISODate);
		e.type = parts[1].toInt();
		e.number = parts[2];
		e.info = parts[3];

		/* Tail after the 4th comma: "duration[,uri[,count]]" —
		 * the uri column was appended later, and count after
		 * that. Both are absent in files from older versions. */
		QString tail = parts.size() > 4 ? parts[4] : QString();
		QStringList tailParts = tail.split(',');
		e.duration = tailParts.size() > 0
			? tailParts[0].toUInt() : 0;
		e.uri = tailParts.size() > 1
			? tailParts[1] : QString();
		e.count = tailParts.size() > 2
			? tailParts[2].toInt() : 1;

		/* Unescape commas in info. */
		e.info.replace("\\,", ",");

		entries_.append(e);
	}

	f.close();
}


void CallHistory::save() const
{
	QDir().mkpath(QFileInfo(filePath()).absolutePath());

	QFile f(filePath());
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return;

	QTextStream out(&f);
	for (const CallHistoryEntry &e : entries_) {
		QString info = e.info;
		info.replace(",", "\\,");
		out << e.ts.toString(Qt::ISODate) << ","
		    << e.type << ","
		    << e.number << ","
		    << info << ","
		    << e.duration;
		if (!e.uri.isEmpty() || e.count > 1)
			out << "," << e.uri << "," << e.count;
		out << "\n";
	}

	out.flush();
	f.close();
}


void CallHistory::add(const QString &number, const QString &uri,
		      int type, const QString &info)
{
	/* If an existing record matches this peer (same number, or
	 * same uri when no number), increment its count and update
	 * the timestamp. This collapses repeat calls to the same
	 * peer into one history entry. */
	for (int i = entries_.size() - 1; i >= 0; --i) {
		CallHistoryEntry &e = entries_[i];
		bool match = false;
		if (!number.isEmpty())
			match = (e.number == number);
		else if (!uri.isEmpty())
			match = (e.uri == uri || e.number.isEmpty());
		if (match) {
			e.ts = QDateTime::currentDateTime();
			e.type = type;
			e.count++;
			e.duration = 0;
			save();
			emit changed();
			return;
		}
	}

	CallHistoryEntry e;
	e.ts   = QDateTime::currentDateTime();
	e.type = type;
	e.number = number;
	e.uri  = uri;
	e.info = info;
	e.duration = 0;
	e.count = 1;

	entries_.append(e);

	/* Keep the file bounded -- last 200 entries. */
	while (entries_.size() > 200)
		entries_.removeFirst();

	save();
	emit changed();
}


void CallHistory::updateDuration(const QString &uri, uint32_t duration)
{
	/* Find the most recent entry matching this peer and update
	 * its duration. Entries written before the uri column existed
	 * match on the number instead. Searches backwards. */
	QString num = uriToNumber(uri.toUtf8().constData());
	for (int i = entries_.size() - 1; i >= 0; --i) {
		if (entries_[i].uri == uri ||
		    (entries_[i].uri.isEmpty() &&
		     entries_[i].number == num)) {
			entries_[i].duration = duration;
			save();
			emit changed();
			return;
		}
	}
}


bool CallHistory::remove(const QDateTime &ts, const QString &number,
			 const QString &uri)
{
	for (int i = 0; i < entries_.size(); ++i) {
		const CallHistoryEntry &e = entries_[i];
		if (e.ts == ts && e.number == number && e.uri == uri) {
			entries_.removeAt(i);
			save();
			emit changed();
			return true;
		}
	}
	return false;
}


QList<CallHistoryEntry> CallHistory::recent(int n) const
{
	QList<CallHistoryEntry> r;
	int start = entries_.size() - n;
	if (start < 0)
		start = 0;
	for (int i = start; i < entries_.size(); ++i)
		r.append(entries_[i]);
	return r;
}
