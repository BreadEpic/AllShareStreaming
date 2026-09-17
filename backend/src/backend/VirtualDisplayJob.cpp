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

#include "backend/VirtualDisplayJob.h"

#include "backend/streambackend/NativeProbeService.h"
#include "common/Logger.h"
#include "server/AppSettings.h"
#include "streaming/ConsoleSession.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QUrl>

namespace {

constexpr int kTransferTimeoutMs = 60 * 1000;
constexpr int kExtractTimeoutMs = 60 * 1000;
constexpr int kHelperTimeoutMs = 5 * 60 * 1000;
constexpr int kPollIntervalMs = 500;

const char* stateName(VirtualDisplayJob::State s)
{
    switch (s) {
    case VirtualDisplayJob::State::Idle: return "idle";
    case VirtualDisplayJob::State::Downloading: return "downloading";
    case VirtualDisplayJob::State::Verifying: return "verifying";
    case VirtualDisplayJob::State::Staging: return "staging";
    case VirtualDisplayJob::State::Elevating: return "elevating";
    case VirtualDisplayJob::State::Installing: return "installing";
    case VirtualDisplayJob::State::Configuring: return "configuring";
    case VirtualDisplayJob::State::Refreshing: return "refreshing";
    case VirtualDisplayJob::State::Done: return "done";
    case VirtualDisplayJob::State::Failed: return "failed";
    }
    return "idle";
}

/// The pinned URL is a constant, but the rule that a driver only ever comes
/// from GitHub is worth stating where the download happens (UpdateChecker
/// keeps the same rule for the update relay).
bool isGitHubUrl(const QUrl& u)
{
    if (u.scheme() != QLatin1String("https")) return false;
    const QString host = u.host().toLower();
    return host == QLatin1String("github.com") || host.endsWith(QLatin1String(".github.com")) ||
           host == QLatin1String("objects.githubusercontent.com") ||
           host.endsWith(QLatin1String(".githubusercontent.com"));
}

QString sha256Hex(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

/// The helper's verdict is its last stdout line; anything before it is noise.
QByteArray lastLine(const QByteArray& out)
{
    const QList<QByteArray> lines = out.trimmed().split('\n');
    return lines.isEmpty() ? QByteArray() : lines.last().trimmed();
}

QString fileSha256(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) return QString();
    return QString::fromLatin1(h.result().toHex());
}

} // namespace

VirtualDisplayJob& VirtualDisplayJob::instance()
{
    static VirtualDisplayJob job;
    return job;
}

VirtualDisplayJob::VirtualDisplayJob(QObject* parent)
    : QObject(parent)
{
    m_Poll.setInterval(kPollIntervalMs);
    connect(&m_Poll, &QTimer::timeout, this, &VirtualDisplayJob::pollResult);
    m_Deadline.setSingleShot(true);
    connect(&m_Deadline, &QTimer::timeout, this, [this]() {
        m_Poll.stop();
        fail(QStringLiteral("The elevated helper did not answer in time"));
    });
}

bool VirtualDisplayJob::running() const
{
    return m_State != State::Idle && m_State != State::Done && m_State != State::Failed;
}

void VirtualDisplayJob::setState(State s)
{
    m_State = s;
    Logger::info(QStringLiteral("[vdisplay] %1").arg(QLatin1String(stateName(s))));
}

QJsonObject VirtualDisplayJob::statusJson() const
{
    QJsonObject obj;
    obj["state"] = QLatin1String(stateName(m_State));
    obj["action"] = m_Request.action == VirtualDisplay::Request::Action::Add
                        ? QStringLiteral("add")
                        : QStringLiteral("remove");
    if (m_StartedAt.isValid()) obj["started_at"] = m_StartedAt.toString(Qt::ISODate);
    if (m_FinishedAt.isValid()) obj["finished_at"] = m_FinishedAt.toString(Qt::ISODate);
    if (m_State == State::Failed) obj["error"] = m_Error;
    if (m_State == State::Done) {
        obj["reboot_required"] = m_RebootRequired;
        if (!m_Display.isEmpty()) obj["display"] = m_Display;
    }
    return obj;
}

QString VirtualDisplayJob::start(const VirtualDisplay::Request& req)
{
    if (running()) return QStringLiteral("A virtual display operation is already running");

    const VirtualDisplay::Status st = VirtualDisplay::probe();
    if (!st.supported) return QStringLiteral("Virtual displays are not supported on this platform");
    if (!st.canInstall)
        return QStringLiteral("No way to elevate on this install — add the driver by hand from %1")
            .arg(VirtualDisplay::downloadUrl());
    if (req.action == VirtualDisplay::Request::Action::Remove && !st.installed)
        return QStringLiteral("No virtual display driver is installed");

    m_Request = req;
    m_Error.clear();
    m_RebootRequired = false;
    m_Display.clear();
    m_StartedAt = QDateTime::currentDateTimeUtc();
    m_FinishedAt = QDateTime();
    m_HelperOut.clear();
    m_ModeStagePending = false;

    if (!QDir().mkpath(VirtualDisplay::stagingDir())) {
        setState(State::Failed);
        m_Error = QStringLiteral("Cannot create the staging directory");
        return m_Error;
    }
    // Leftovers of an interrupted attempt would be acted on as-is otherwise.
    QFile::remove(VirtualDisplay::resultPath());
    QFile::remove(VirtualDisplay::requestPath());

    if (req.action == VirtualDisplay::Request::Action::Remove || st.installed) {
        // Removing needs no download; and a driver already present (installed
        // by hand, or by us earlier) is never re-fetched: the helper only
        // configures what is there.
        dispatch();
    } else {
        download();
    }
    return QString();
}

void VirtualDisplayJob::fail(const QString& error)
{
    m_Error = error;
    m_FinishedAt = QDateTime::currentDateTimeUtc();
    m_Poll.stop();
    m_Deadline.stop();
    QFile::remove(VirtualDisplay::requestPath());
    QDir(VirtualDisplay::driverDir()).removeRecursively();
    QFile::remove(VirtualDisplay::archivePath());
    if (m_Nam) {
        m_Nam->deleteLater();
        m_Nam = nullptr;
    }
    Logger::warning(QStringLiteral("[vdisplay] failed: %1").arg(error));
    setState(State::Failed);
}

void VirtualDisplayJob::succeed(const VirtualDisplay::Result& res)
{
    m_RebootRequired = res.rebootRequired;
    m_Display = res.display;
    m_FinishedAt = QDateTime::currentDateTimeUtc();
    QFile::remove(VirtualDisplay::requestPath());
    QDir(VirtualDisplay::driverDir()).removeRecursively();
    QFile::remove(VirtualDisplay::archivePath());

    AppSettings settings;
    if (m_Request.action == VirtualDisplay::Request::Action::Add) {
        QJsonObject rec;
        rec["added"] = true;
        rec["width"] = m_Request.width;
        rec["height"] = m_Request.height;
        rec["refresh"] = m_Request.refresh;
        rec["hdr"] = m_Request.hdr;
        rec["gpu"] = m_Request.gpu;
        rec["added_at"] = m_FinishedAt.toString(Qt::ISODate);
        settings.setVirtualDisplay(rec);
    } else {
        settings.clearVirtualDisplay();
    }
    setState(State::Done);
}

// ── Download ────────────────────────────────────────────────────────────────

void VirtualDisplayJob::download()
{
    setState(State::Downloading);
    const QUrl url(VirtualDisplay::downloadUrl());
    if (!isGitHubUrl(url)) {
        fail(QStringLiteral("The driver URL is not a GitHub address"));
        return;
    }

    m_Nam = new QNetworkAccessManager(this);
    QNetworkRequest req{url};
    req.setRawHeader("User-Agent", "MoonlightWeb");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(kTransferTimeoutMs);

    Logger::info(QStringLiteral("[vdisplay] downloading %1").arg(url.toString()));
    QNetworkReply* reply = m_Nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("Download failed: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray payload = reply->readAll();
        setState(State::Verifying);
        // Verified BEFORE it is written anywhere: an elevated process will act
        // on these bytes. A mismatch is a mismatch — a replaced asset, a proxy
        // that rewrote the download, an HTML error page — none is "try anyway".
        const QString digest = sha256Hex(payload);
        if (digest != VirtualDisplay::downloadSha256()) {
            fail(QStringLiteral("The downloaded driver package does not match the published one "
                                "(%1 bytes, sha256 %2…)")
                     .arg(payload.size())
                     .arg(digest.left(12)));
            return;
        }
        QFile file(VirtualDisplay::archivePath());
        if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()) {
            fail(QStringLiteral("Cannot write to the staging directory"));
            return;
        }
        file.close();
        m_Nam->deleteLater();
        m_Nam = nullptr;
        extract();
    });
}

// ── Extract + per-file verification ─────────────────────────────────────────

void VirtualDisplayJob::extract()
{
    setState(State::Staging);
    QDir(VirtualDisplay::driverDir()).removeRecursively();
    if (!QDir().mkpath(VirtualDisplay::driverDir())) {
        fail(QStringLiteral("Cannot create the driver directory"));
        return;
    }

    // Qt has no zip reader; Windows has shipped bsdtar as tar.exe since 1803,
    // well below the engine's own floor. Called by its full path: the staging
    // directory is not on PATH and nothing here searches one.
    const QString tar = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")) +
                        QStringLiteral("/System32/tar.exe");
    QProcess* proc = new QProcess(this);
    QTimer* deadline = new QTimer(proc);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, proc, [proc]() { proc->kill(); });
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            fail(QStringLiteral("Could not extract the driver package (tar exit %1)").arg(code));
            return;
        }
        for (const VirtualDisplay::DriverFile& f : VirtualDisplay::driverFiles()) {
            const QString path =
                VirtualDisplay::driverDir() + QLatin1Char('/') + QLatin1String(f.name);
            const QString got = fileSha256(path);
            if (got != QLatin1String(f.sha256)) {
                fail(
                    QStringLiteral("%1 does not match its pinned hash").arg(QLatin1String(f.name)));
                return;
            }
        }
        dispatch();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        proc->deleteLater();
        fail(QStringLiteral("tar.exe could not be started"));
    });
    deadline->start(kExtractTimeoutMs);
    proc->start(tar,
                {QStringLiteral("-xf"), QDir::toNativeSeparators(VirtualDisplay::archivePath()),
                 QStringLiteral("-C"), QDir::toNativeSeparators(VirtualDisplay::driverDir())});
}

// ── Reaching the helper ─────────────────────────────────────────────────────

void VirtualDisplayJob::dispatch()
{
    QFile req(VirtualDisplay::requestPath());
    if (!req.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("Cannot write the request file"));
        return;
    }
    req.write(VirtualDisplay::toJson(m_Request));
    req.close();

    setState(State::Elevating);
    const QString method = VirtualDisplay::probe().method;
    const QStringList dirArg = {QStringLiteral("--vdisplay-dir"),
                                QDir::toNativeSeparators(VirtualDisplay::stagingDir())};

    if (method == QLatin1String("task")) {
        runTask();
    } else if (method == QLatin1String("elevated")) {
        if (!qEnvironmentVariableIsEmpty("MW_SERVICE") &&
            m_Request.action == VirtualDisplay::Request::Action::Add) {
            // Session 0 can install the driver but has no desktop to set a
            // mode on: the mode stage follows in the console session.
            m_ModeStagePending = true;
            runHelper(QStringList{VirtualDisplay::applyArgument(), QStringLiteral("--stage=driver")}
                          << dirArg,
                      false);
        } else {
            runHelper(QStringList{VirtualDisplay::applyArgument(), QStringLiteral("--stage=all")}
                          << dirArg,
                      false);
        }
    } else {
        fail(QStringLiteral("No way to elevate on this install"));
    }
}

void VirtualDisplayJob::runTask()
{
    // The installer's trigger-less elevated task: started by name, it runs our
    // exe with --vdisplay-apply as this user, elevated, hidden. Its result
    // reaches us through the result file — there is no stdout to read.
    QProcess* proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            fail(QStringLiteral("schtasks could not start \"%1\" (exit %2)")
                     .arg(VirtualDisplay::taskName())
                     .arg(code));
            return;
        }
        setState(State::Installing);
        m_Deadline.start(kHelperTimeoutMs);
        m_Poll.start();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        proc->deleteLater();
        fail(QStringLiteral("schtasks.exe could not be started"));
    });
    proc->start(QStringLiteral("schtasks.exe"),
                {QStringLiteral("/Run"), QStringLiteral("/TN"), VirtualDisplay::taskName()});
}

void VirtualDisplayJob::runHelper(const QStringList& args, bool inConsoleSession)
{
    setState(m_ModeStagePending && inConsoleSession ? State::Configuring : State::Installing);
    m_HelperOut.clear();
    const QString exe = QCoreApplication::applicationFilePath();

    if (inConsoleSession) {
        ConsoleProcess* cp = new ConsoleProcess(this);
        connect(cp, &ConsoleProcess::stdoutData, this,
                [this](const QByteArray& d) { m_HelperOut += d; });
        connect(cp, &ConsoleProcess::finished, this, [this, cp](int code, bool crashed) {
            cp->deleteLater();
            m_Deadline.stop();
            const auto res = VirtualDisplay::parseResult(lastLine(m_HelperOut));
            if (crashed || !res) {
                fail(QStringLiteral("The console-session helper ended without a result (exit %1)")
                         .arg(code));
                return;
            }
            handleResult(*res);
        });
        QString error;
        if (!cp->start(exe, args, &error)) {
            cp->deleteLater();
            fail(
                QStringLiteral("Could not start the helper in the console session: %1").arg(error));
            return;
        }
        m_Deadline.start(kHelperTimeoutMs);
        return;
    }

    QProcess* proc = new QProcess(this);
    connect(proc, &QProcess::readyReadStandardOutput, this,
            [this, proc]() { m_HelperOut += proc->readAllStandardOutput(); });
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        m_Deadline.stop();
        m_HelperOut += proc->readAllStandardOutput();
        const auto res = VirtualDisplay::parseResult(lastLine(m_HelperOut));
        if (status != QProcess::NormalExit || !res) {
            fail(QStringLiteral("The elevated helper ended without a result (exit %1)").arg(code));
            return;
        }
        handleResult(*res);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        proc->deleteLater();
        fail(QStringLiteral("The helper could not be started"));
    });
    m_Deadline.start(kHelperTimeoutMs);
    proc->start(exe, args);
}

void VirtualDisplayJob::pollResult()
{
    QFile f(VirtualDisplay::resultPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto res = VirtualDisplay::parseResult(f.readAll());
    if (!res) return; // still being written
    m_Poll.stop();
    m_Deadline.stop();
    handleResult(*res);
}

void VirtualDisplayJob::handleResult(const VirtualDisplay::Result& res)
{
    QFile::remove(VirtualDisplay::resultPath());
    if (!res.ok) {
        fail(res.error.isEmpty() ? QStringLiteral("The helper failed at stage %1").arg(res.stage)
                                 : res.error);
        return;
    }
    if (m_ModeStagePending) {
        // Driver half done as SYSTEM; the desktop half runs as the console user.
        m_ModeStagePending = false;
        m_RebootRequired = res.rebootRequired;
        runHelper({VirtualDisplay::applyArgument(), QStringLiteral("--stage=mode"),
                   QStringLiteral("--vdisplay-dir"),
                   QDir::toNativeSeparators(VirtualDisplay::stagingDir())},
                  true);
        return;
    }
    VirtualDisplay::Result merged = res;
    merged.rebootRequired = merged.rebootRequired || m_RebootRequired;
    setState(State::Refreshing);
    refreshEngine();
    succeed(merged);
}

void VirtualDisplayJob::refreshEngine()
{
    // The display list moved under the engine: ask again. As a service this
    // spawns the console probe; on a desktop it is a direct call. Either way
    // NativeProbeService::changed() carries the news to ComputerManager, which
    // rebuilds the native host card.
    NativeProbeService::instance().refresh();
}
