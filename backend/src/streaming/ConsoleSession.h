/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

/**
 * The interactive desktop, seen from a process that may not be on it.
 *
 * MoonlightWeb's standard installation is a Windows service (NSSM, session 0).
 * Session 0 has no desktop anybody looks at: DXGI Desktop Duplication has
 * nothing to duplicate there, SendInput reaches no window, and the native
 * engine's own probe says so in one line ("no interactive desktop session").
 * Everything the native host does therefore has to happen in the CONSOLE
 * session — the one with the monitor and the keyboard — in a process that
 * runs as the user logged on there.
 *
 * This is how Sunshine's service works too, with one difference that is
 * deliberate: Sunshine puts SYSTEM into the user's session, so it can capture
 * the UAC secure desktop and type into elevated windows. MoonlightWeb runs the
 * worker as the logged-on USER (their full, elevated token when UAC gives them
 * one, the plain token otherwise). A network-facing process that decodes input
 * from a browser is exactly the process to hold the least privilege it can, and
 * the price — a black picture while a UAC prompt is up, which the capture loop
 * already handles as a lost display — is one Sunshine users pay in a different
 * form (the prompt is captured, but the stream cannot answer it either).
 *
 * Nothing here runs when the process already sits on the desktop (a dev
 * instance, a manual launch): launchesElsewhere() is false, and the worker is
 * spawned with QProcess as it always was.
 */
namespace ConsoleSession {

/// Where the desktop is, relative to this process, and who is on it.
struct Info
{
    /// This process cannot reach the desktop itself (Windows service in
    /// session 0). Every native launch has to go through ConsoleProcess.
    bool elsewhere = false;

    /// Somebody is logged on at the console — locked or not. False between
    /// logoff and the next logon, and on a machine that booted to the login
    /// screen and stayed there.
    bool userPresent = false;

    /// The console session id (WTSGetActiveConsoleSessionId). 0 when unknown.
    quint32 sessionId = 0;

    /// Who is logged on there. Empty when nobody is.
    QString userName;

    /// True when the token the worker would run under is the user's elevated
    /// (UAC "linked") token — an administrator's full rights, which is what
    /// lets input reach an elevated game. Informational.
    bool elevated = false;

    /// English, for the log and the status route: why userPresent is false,
    /// or why a launch cannot work from here (a service account without the
    /// right to enter the console session). Empty when everything is fine.
    QString reason;
};

/// Whether a native session started by this process has to be launched into
/// the console session rather than run here. Decided once: it is a property of
/// how the process was started. `MW_CONSOLE_LAUNCH=force` answers true from an
/// ordinary desktop session, for testing the launcher without a service.
bool launchesElsewhere();

/// Look at the console session without spawning anything. Cheap enough to
/// poll every few seconds.
Info query();

/// The trigger-less, RunLevel=HighestAvailable scheduled task that starts this
/// executable as a stream worker with the user's FULL token — the installer
/// registers it, because only an elevated process may. Per edition and per
/// --dev, like every other registration ("MoonlightWeb Stream Worker").
QString elevatedWorkerTaskName();

/// Whether a native worker started from this desktop process should go through
/// that task rather than QProcess: Windows, on the desktop (a service has its
/// own way in, see launchesElsewhere()), not already elevated, and the task is
/// there with THIS executable as its action — so a build tree never launches
/// the installed binary's worker, nor the other way round. False for the rest
/// of the run once a launch through it has failed.
///
/// Why at all: an unelevated worker is shut out by Windows (UIPI) from every
/// window that runs as administrator — Task Manager, an admin console, a game
/// started elevated — and not just that window: while one has the focus, ALL
/// injected input is dropped. The logged-on user can type there; their remote
/// self should too. Only the worker is raised, never the server: the process
/// that listens on the network, draws the tray and opens the browser stays at
/// the user's ordinary level.
bool elevatedWorkerAvailable();

/// Worker side of ConsoleProcess::startThroughTask(): open the three pipes
/// named after `base`, check that the process at the other end is this same
/// executable, and make them this process's stdin, stdout and stderr — CRT and
/// Win32 both — so the worker reads and writes them exactly as it would the
/// pipes of a QProcess parent. Must run before anything touches std::cin.
bool attachWorkerPipes(const QString& base, QString* error);

} // namespace ConsoleSession

/**
 * A child process of this executable, running in the console session as the
 * user logged on there, with its stdin/stdout/stderr piped back here.
 *
 * The QProcess-shaped subset StreamWorkerHost needs, and nothing more:
 * QProcess itself cannot do this (it calls CreateProcess, which has no token
 * parameter), and the plumbing — a token from WTSQueryUserToken, the user's
 * environment block, `winsta0\default`, three pipes whose child ends are the
 * ONLY handles the child inherits — is the whole point of the class.
 *
 * Signals are delivered on this object's thread; the pipes are drained by
 * threads of their own. finished() is emitted only after both output pipes
 * have been read to their end, so a "response" line the child wrote just
 * before exiting is delivered before the exit is.
 *
 * Only Windows can do any of this. On other platforms start() fails with a
 * reason, and launchesElsewhere() is false anyway.
 */
class ConsoleProcess : public QObject
{
    Q_OBJECT

public:
    explicit ConsoleProcess(QObject* parent = nullptr);
    ~ConsoleProcess() override;

    /// Launch `program args...` in the console session. Returns false and fills
    /// `error` when it could not be done — nobody logged on, no right to their
    /// token, CreateProcessAsUser refused.
    bool start(const QString& program, const QStringList& args, QString* error);

    /// Launch a stream worker through the scheduled task `taskName` (see
    /// ConsoleSession::elevatedWorkerAvailable()), in this very session, as the
    /// user at their highest level. A task cannot hand a child our pipes, so
    /// they are NAMED pipes this time — random names, a DACL that admits the
    /// user alone, one instance each — passed as the task's $(Arg0); the
    /// worker connects back (attachWorkerPipes). Blocks until it has, a few
    /// hundred milliseconds, 10 s at most. The worker's image is checked
    /// before a byte is written to it.
    ///
    /// kill() terminates the worker when Windows grants that right on it, and
    /// otherwise closes its stdin, which the worker reads as "the parent is
    /// gone" and exits within seconds.
    bool startThroughTask(const QString& taskName, QString* error);

    /// Write to the child's stdin. Dropped when the child is gone.
    void write(const QByteArray& data);

    bool isRunning() const;
    void kill();
    qint64 processId() const;

    /// Who the child runs as, once started ("bruno", or "(this session)" when
    /// forced for testing).
    QString userName() const;

signals:
    void stdoutData(const QByteArray& data);
    void stderrData(const QByteArray& data);
    /// The child exited. `crashed` mirrors QProcess::CrashExit: an exit code
    /// that is an NTSTATUS failure rather than a return value.
    void finished(int exitCode, bool crashed);

private:
    /// Readers for stdout/stderr and the exit waiter, once the child is up.
    void beginDraining();

    struct Impl;
    std::unique_ptr<Impl> d;
};
