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
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return;

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
		e.uri  = parts[2];
		e.info = parts[3];
		e.duration = parts.size() > 4 ? parts[4].toUInt() : 0;

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
		    << e.uri << ","
		    << info << ","
		    << e.duration << "\n";
	}

	out.flush();
	f.close();
}


void CallHistory::add(const QString &uri, int type, const QString &info)
{
	CallHistoryEntry e;
	e.ts   = QDateTime::currentDateTime();
	e.type = type;
	e.uri  = uri;
	e.info = info;
	e.duration = 0;

	entries_.append(e);

	/* Keep the file bounded -- last 200 entries. */
	while (entries_.size() > 200)
		entries_.removeFirst();

	save();
	emit changed();
}


void CallHistory::updateDuration(const QString &uri, uint32_t duration)
{
	/* Find the most recent entry matching this URI and update
	 * its duration. Searches backwards from the end. */
	for (int i = entries_.size() - 1; i >= 0; --i) {
		if (entries_[i].uri == uri) {
			entries_[i].duration = duration;
			save();
			emit changed();
			return;
		}
	}
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
