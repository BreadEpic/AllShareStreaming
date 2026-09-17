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
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>

/**
 * @brief A virtual display for a headless native host: the facts, the pinned
 * constants and the pure rules.
 *
 * ── Why this exists ─────────────────────────────────────────────────────────
 *
 * A PC installed through a TV and then left without a screen has nothing to
 * stream: the native host card used to vanish (ComputerManager) and the wizard
 * sent the owner to install Sunshine. The fix is a virtual display the admin
 * adds from the browser — on a machine that, by definition, nobody is sitting
 * at.
 *
 * ── Windows: a third-party signed driver, fetched on demand ────────────────
 *
 * Windows has no virtual display of its own; an Indirect Display (IddCx)
 * driver is the only way, and one we write ourselves could not be signed by
 * this project's CI (Microsoft attestation needs an EV certificate). So the
 * driver is VirtualDrivers/Virtual-Display-Driver — MIT, SignPath-signed,
 * maintained — and it is NEVER bundled: the exact release asset is pinned by
 * URL and SHA-256 here, downloaded when the admin clicks, verified before a
 * byte touches the disk, and verified again by the elevated helper before
 * SetupAPI sees it. The same contract as the ViGEmBus install (GamepadDriver).
 *
 * ── Elevation without anyone at the desk ───────────────────────────────────
 *
 * The production server runs unprivileged (logon task, LeastPrivilege). A UAC
 * prompt would appear on a desktop with no monitor attached, so the installer
 * (which is elevated) registers a trigger-less task, RunLevel=HighestAvailable,
 * whose fixed action is OUR exe with `--vdisplay-apply`. The server writes a
 * request file holding parameters only — never a path to run — starts the
 * task, and reads the result file. This is the pattern of the "Update" task,
 * with one improvement: the command is the signed exe under Program Files,
 * not a file that was downloaded.
 *
 * ── Access ──────────────────────────────────────────────────────────────────
 *
 * Deliberately NOT the loopback-only rule of GamepadDriver::mayOffer: a
 * headless server is managed from another machine or there is no point. The
 * gate is the admin privilege (HttpRequest::isLocal — host machine, or a
 * session that unlocked the remote admin password, LAN or tunnel), one notch
 * stricter than /api/update/start which is merely session-authenticated.
 *
 * Everything in this header that has no OS dependency (mayManage, the request
 * parser, the settings XML, the task name) is pure and covered by
 * tests/test_virtual_display.cpp on every platform.
 */
namespace VirtualDisplay {

// ── Pinned upstream asset (Windows x64) ────────────────────────────────────
//
// VirtualDrivers/Virtual-Display-Driver release 25.7.23, the bare driver
// package (inf + cat + dll + a sample settings file), 132 KB. The "x86" in the
// asset name is upstream's label for the x64 build (its INF is NTamd64).
// Bumping the version means recomputing every hash below from the published
// file; a URL bumped alone fails every install rather than trusting new bytes.

inline QString downloadUrl()
{
    return QStringLiteral("https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/"
                          "download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip");
}

/// SHA-256 of the zip, lowercase hex — matches GitHub's published digest.
inline QString downloadSha256()
{
    return QStringLiteral("e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a");
}

/// One file of the package as it must be found after extraction.
struct DriverFile
{
    const char* name;   ///< path inside the zip
    const char* sha256; ///< lowercase hex
    bool required;      ///< the install refuses without it
};

/// The three files SetupAPI consumes. The sample vdd_settings.xml in the zip is
/// not listed: it is replaced by ours (settingsXml) and never trusted.
inline const std::array<DriverFile, 3>& driverFiles()
{
    static const std::array<DriverFile, 3> files = {{
        {"VirtualDisplayDriver/MttVDD.inf",
         "550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d", true},
        {"VirtualDisplayDriver/mttvdd.cat",
         "08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a918dc859468f6", true},
        {"VirtualDisplayDriver/MttVDD.dll",
         "c9ca837f57a98fbd43bc416a7f535a95843626e7759eaf85cf0cd7ce334dbb05", true},
    }};
    return files;
}

/// The root-enumerated hardware id the INF binds to.
inline QString hardwareId()
{
    return QStringLiteral("Root\\MttVDD");
}

/// SHA-1 thumbprint of the certificate that signed the catalog (SignPath
/// Foundation, issued by GlobalSign, valid to 2027-09-07). A user-mode driver
/// package installs silently only when its publisher is trusted: the elevated
/// helper adds THIS certificate — found inside the catalog, and only if its
/// thumbprint matches — to the machine's Trusted Publishers for the duration of
/// the install, then removes it again. Nothing signed by anyone else is ever
/// trusted, and the trust does not outlive the operation.
inline QString catalogSignerThumbprint()
{
    return QStringLiteral("3cf8cf26d8ba266c3a483ab7d26d4a818e317d76");
}

/// Where the driver reads its configuration (fixed by the driver, not by us).
inline QString settingsXmlPath()
{
    return QStringLiteral("C:/VirtualDisplayDriver/vdd_settings.xml");
}

// ── Presets ─────────────────────────────────────────────────────────────────

struct Preset
{
    int width;
    int height;
};

inline const std::array<Preset, 4>& resolutionPresets()
{
    static const std::array<Preset, 4> p = {
        {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}}};
    return p;
}

inline const std::array<int, 3>& refreshPresets()
{
    static const std::array<int, 3> r = {{60, 90, 120}};
    return r;
}

// ── Task naming ─────────────────────────────────────────────────────────────

/// The elevated task's name for an edition: "MoonlightWeb Virtual Display",
/// "MoonlightWebDev Virtual Display", "MoonlightWeb-dev Virtual Display" for a
/// --dev instance (which the installer never registers: --dev is a scratch
/// instance and gets the manual path).
QString taskNameFor(const QString& productName, bool devFlag);

/// taskNameFor() for this process.
QString taskName();

/// The fixed argument the task runs our exe with.
inline QString applyArgument()
{
    return QStringLiteral("--vdisplay-apply");
}

// ── Request / result files ──────────────────────────────────────────────────

struct Request
{
    enum class Action
    {
        Add,
        Remove
    };
    Action action = Action::Add;
    int width = 1920;
    int height = 1080;
    int refresh = 60;
    bool hdr = false;
    QString gpu; ///< adapter friendly name, empty = the driver's default
};

/// Parse and validate a request. Only the presets are accepted: the file is
/// read by an elevated process, so its contents are a set of choices, not
/// free-form input. Returns nullopt with @p error filled on any deviation.
std::optional<Request> parseRequest(const QByteArray& json, QString* error);
QByteArray toJson(const Request& req);

struct Result
{
    bool ok = false;
    QString stage; ///< where it ended: "driver" | "mode" | "done"
    QString error; ///< English, for the log and the dialog
    bool rebootRequired = false;
    QString display; ///< GDI name of the display that came up ("\\.\DISPLAY3")
};

/// Tolerant of an empty or partial file: the server polls while the helper is
/// still writing. Returns nullopt until a complete object is there.
std::optional<Result> parseResult(const QByteArray& json);
QByteArray toJson(const Result& res);

/// The driver's vdd_settings.xml for one request: a single monitor, the chosen
/// mode first (the driver picks the first as default), every preset after it
/// so a later mode change never needs the driver reloaded, HDR on request.
QString settingsXml(const Request& req);

// ── Paths ───────────────────────────────────────────────────────────────────

/// Per-user staging directory. Not %TEMP%: it holds files an elevated process
/// will act on, and C:\Windows\Temp is writable by every local account.
QString stagingDir();
QString requestPath();
QString resultPath();
QString driverDir();
QString archivePath();

// ── Status ──────────────────────────────────────────────────────────────────

struct Gpu
{
    int id = -1;
    QString name;
};

struct ActiveDisplay
{
    int id = -1;
    QString label;
    int width = 0;
    int height = 0;
    int refreshMilliHz = 0;
    bool hdrActive = false;
};

struct Status
{
    bool supported = false;  ///< this platform can have a virtual display added
    bool installed = false;  ///< the driver's device node exists
    bool active = false;     ///< a virtual display is in the probe's list
    bool canInstall = false; ///< an elevation path exists (see method)
    QString method;          ///< "task" | "elevated" | ""
    bool osHdrCapable = false;
    QList<ActiveDisplay> activeDisplays;
    QList<Gpu> gpus;
};

/// The one access rule, pure so it is tested without a driver or a socket:
/// the caller holds the admin privilege and the platform supports the feature.
inline bool mayManage(bool adminPrivilege, const Status& status)
{
    return adminPrivilege && status.supported;
}

/// Probe everything. Cheap except for the Task Scheduler lookup, which is
/// cached for a while — nothing there changes without an install.
Status probe();

/// Serialise for the REST API. @p admin adds the parts that describe the
/// machine (paths, GPUs, elevation) — a remote viewer gets availability only.
QJsonObject toJson(const Status& st, bool admin);

/// Windows: does the driver's device node exist right now?
bool driverPresent();

/// Windows: is the process token elevated?
bool processElevated();

} // namespace VirtualDisplay
