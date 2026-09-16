/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What the host keeps of its own log. It was a pure append with no ceiling
 * until 16/09/2026: 472 MB over 70 days on the machine running longest, a
 * median of 7.4 MB a day, of which 69% was the per-host `serverinfo` poll that
 * runs every ~7 s whether or not anyone streams. Nothing there is
 * developer-specific — a one-host install writes that same poll — so an
 * ordinary user was on course for roughly a gigabyte a year on their system
 * disk.
 *
 * The promise is a DURATION: seven days of history. Size is the consequence and
 * follows the number of hosts actually polled, because a host that stops
 * answering stops writing lines. kMaxTotalBytes is not the budget but a brake,
 * for the one case a duration cannot defend against — a host logging abnormally
 * would fill a disk long before seven days were up.
 *
 * What these tests hold: the live file stays bounded, archives expire on AGE
 * (not on a count, and not on size while the disk is fine), the brake trims
 * oldest-first when it is hit, both say so in the log, a host upgrading from
 * the unbounded era gets its disk back on the first open, the archive glob
 * never touches a neighbouring worker or probe log, and the two callers whose
 * file is NOT a long-lived server log — an operator's --log path, a
 * per-session worker — are left completely alone.
 */
#include "test_framework.h"

#include "common/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <iostream>
#include <sstream>

namespace {

qint64 sizeOf(const QString& path)
{
    return QFileInfo(path).size();
}

void writeLines(int count)
{
    const QString filler(200, QLatin1Char('x'));
    for (int i = 0; i < count; ++i)
        Logger::info(filler);
}

/// Write until the live file is seen to shrink — exactly one rollover, however
/// the roll point and the size-check interval happen to line up.
bool fillUntilRollover(const QString& path)
{
    const QString filler(200, QLatin1Char('x'));
    qint64 prev = sizeOf(path);
    for (int i = 0; i < 20000; ++i) {
        Logger::info(filler);
        const qint64 now = sizeOf(path);
        if (now < prev) return true;
        prev = now;
    }
    return false;
}

bool fileContains(const QString& path, const QString& needle)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QByteArray all = f.readAll();
    f.close();
    return all.contains(needle.toUtf8());
}

/// Archives beside `log`, i.e. everything named `log.*`.
QStringList archivesOf(const QString& log)
{
    const QFileInfo info(log);
    return info.dir().entryList({info.fileName() + QStringLiteral(".*")}, QDir::Files);
}

/// Age a file by moving its modification time back. Retention is measured on
/// that timestamp, so this buys days of history without waiting for them —
/// sleeping through a real window would make the suite unrunnable.
bool backdate(const QString& path, int secs)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) return false;
    const bool ok = f.setFileTime(QDateTime::currentDateTime().addSecs(-secs),
                                  QFileDevice::FileModificationTime);
    f.close();
    return ok;
}

/// Logger echoes every INFO to stdout. Filling a log several times over means
/// thousands of lines of filler, which would bury the suite's own output — so
/// stdout is parked on a sink for the duration. The framework writes to stderr
/// and is unaffected.
class MutedStdout
{
public:
    MutedStdout()
        : m_Saved(std::cout.rdbuf(m_Sink.rdbuf()))
    {}
    ~MutedStdout() { std::cout.rdbuf(m_Saved); }

private:
    std::ostringstream m_Sink;
    std::streambuf* m_Saved;
};

} // namespace

void run_logger_rotation_tests()
{
    SECTION("Logger rotation");
    MutedStdout quiet;

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString log = dir.filePath(QStringLiteral("moonlightweb.log"));

    // Production is 5 MB / 128 MB / 7 days. Scaled down: pushing 128 MB through
    // a path that flushes every line, or waiting a week, would make this suite
    // unrunnable. Retention is an hour here, and files are aged by hand.
    const qint64 kFile = 8 * 1024;
    const qint64 kTotal = 64 * 1024;
    const qint64 kRetentionSecs = 3600;
    Logger::instance()->setBudgetForTests(kFile, kTotal, kRetentionSecs);

    // ── The live file stays bounded, and archives appear ────────────────────
    Logger::instance()->setLogFile(log);
    CHECK(fillUntilRollover(log));
    CHECK(archivesOf(log).size() >= 1);
    // The overshoot is one size-check interval plus one line, never unbounded.
    CHECK(sizeOf(log) < kFile * 2);

    // ── Archives are kept on AGE, not on a count ────────────────────────────
    // A young archive survives however many there are; the disk is fine, so
    // nothing else has any business deleting it.
    {
        CHECK(fillUntilRollover(log));
        CHECK(fillUntilRollover(log));
        const int before = archivesOf(log).size();
        CHECK(before >= 3);

        // Nothing has aged out and the brake is not hit: all of them stay.
        writeLines(5);
        CHECK(archivesOf(log).size() == before);
    }

    // ── An archive past the window is dropped, and the log says so ──────────
    {
        const QStringList names = archivesOf(log);
        CHECK(!names.isEmpty());
        const QString oldest = QFileInfo(log).dir().filePath(names.first());
        CHECK(backdate(oldest, kRetentionSecs * 2)); // older than the window

        const int before = archivesOf(log).size();
        CHECK(fillUntilRollover(log)); // any rollover runs the prune
        const QStringList after = archivesOf(log);

        CHECK(!after.contains(QFileInfo(oldest).fileName())); // the aged one left
        CHECK(after.size() == before);                        // and one fresh archive replaced it
        CHECK(fileContains(log, QStringLiteral("older than")));
    }

    // ── The brake trims oldest-first when the disk is the problem ───────────
    // Retention alone cannot defend against a host logging abnormally fast:
    // nothing is old yet, and the disk fills anyway.
    {
        writeLines(4000); // far more than kTotal, all of it minutes old
        CHECK(Logger::instance()->logFootprintBytes() <= kTotal);
        CHECK(fileContains(log, QStringLiteral("over its")));
    }

    // ── Neighbouring logs are never touched ─────────────────────────────────
    // `moonlightweb.log.*` must not reach a worker's per-PID file or the
    // probe's, which belong to other processes.
    {
        const QString worker = dir.filePath(QStringLiteral("moonlightweb-worker-123.log"));
        const QString probe = dir.filePath(QStringLiteral("moonlightweb-probe.log"));
        for (const QString& p : {worker, probe}) {
            QFile f(p);
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("not ours\n");
            f.close();
            CHECK(backdate(p, kRetentionSecs * 100)); // ancient, and irrelevant
        }

        writeLines(2000); // plenty of rollovers and prunes
        CHECK(QFile::exists(worker));
        CHECK(QFile::exists(probe));
    }

    // ── A host upgrading from the unbounded era gets its disk back at once ──
    {
        const QString legacy = dir.filePath(QStringLiteral("legacy.log"));
        QFile seed(legacy);
        CHECK(seed.open(QIODevice::WriteOnly));
        seed.write(QByteArray(int(kTotal) * 4, 'y')); // far past the brake
        seed.close();
        CHECK(sizeOf(legacy) > kTotal);

        Logger::instance()->setLogFile(legacy);
        CHECK(Logger::instance()->logFootprintBytes() <= kTotal);
        CHECK(sizeOf(legacy) < kFile); // the live file restarted small
    }

    // ── A non-rotating file is left completely alone ────────────────────────
    // An operator's --log path, and a per-session worker's own file: rolling or
    // pruning either would destroy the very capture that was asked for.
    {
        const QString fixed = dir.filePath(QStringLiteral("operator.log"));
        Logger::instance()->setLogFile(fixed, /*rotating=*/false);
        writeLines(600);
        CHECK(sizeOf(fixed) > kTotal); // grew straight past the brake
        CHECK(archivesOf(fixed).isEmpty());
    }

    // Leave the singleton pointing somewhere harmless: QTemporaryDir cannot
    // remove a file this process still holds open.
    Logger::instance()->setBudgetForTests(0, 0, 0); // back to the real budget
    Logger::instance()->setLogFile(QDir::tempPath() + QStringLiteral("/mw-backend-tnr.log"),
                                   /*rotating=*/false);
}
