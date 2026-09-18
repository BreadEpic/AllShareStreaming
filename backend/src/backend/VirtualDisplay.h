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
#include <QString>

#include <array>
#include <optional>

namespace mw::native {
struct DisplayInfo;
}

/**
 * @brief "MoonlightWeb Virtual Display": the screen this machine streams when
 * it has none, or when the one it has is not the one to stream.
 *
 * ── What it is ──────────────────────────────────────────────────────────────
 *
 * A virtual display that belongs to MoonlightWeb, shown on the native host's
 * card as one more app, named after itself. It is OFF by default: opening
 * that card turns it on, makes it the primary display, streams it, and when
 * the last stream on it ends it is turned off again and the previous primary
 * display gets its role back. A PC with monitors keeps them; a headless PC —
 * installed through a TV, then left without a screen — has exactly this one
 * card to stream.
 *
 * ── Windows: a bundled, signed, third-party driver ─────────────────────────
 *
 * Windows has no virtual display of its own; an Indirect Display (IddCx)
 * driver is the only way, and one we write ourselves could not be signed by
 * this project's CI (Microsoft attestation needs an EV certificate). So the
 * driver is VirtualDrivers/Virtual-Display-Driver — MIT, SignPath-signed,
 * maintained — shipped INSIDE the installer (backend/installer/drivers/vdd,
 * three files, 270 KB) and pinned by SHA-256 here: the elevated helper
 * re-hashes every file before SetupAPI sees it, and trusts the catalog's
 * signer only for the duration of the install.
 *
 * The installer asks once (Skip / Accept, like the Internet page), creates a
 * root-enumerated device node for the driver with the friendly name
 * "MoonlightWeb Virtual Display", and disables it. That friendly name is what
 * makes the node OURS: a "VDD by MTT" the owner installed before keeps its
 * node, its settings and its monitor, we never touch them, and an update or a
 * reinstall finds our node and creates no second one. Nothing in the app
 * uninstalls it: only the MoonlightWeb uninstaller (or Device Manager) does.
 *
 * ── Elevation without anyone at the desk ───────────────────────────────────
 *
 * Enabling and disabling a device node needs administrator rights, and the
 * production server runs unprivileged (logon task, LeastPrivilege). A UAC
 * prompt would appear on a desktop nobody may be sitting at, so the installer
 * (which is elevated) registers a trigger-less task, RunLevel=HighestAvailable,
 * whose fixed action is OUR exe with `--vdisplay-apply`. The server writes a
 * request file holding parameters only — never a path to run — starts the
 * task, and reads the result file. The pattern of the "Update" task, with one
 * improvement: the command is the signed exe under Program Files.
 *
 * ── macOS: no driver at all ────────────────────────────────────────────────
 *
 * CoreGraphics can register a display from a process (the private
 * CGVirtualDisplay classes BetterDisplay and DeskPad are built on). The card
 * is always there; opening it creates the display in this process, makes it
 * the main display, and the display goes away with the last stream. The seam
 * is mw::native::vdisplay (native-host/include/mw/native/VirtualDisplay.h);
 * Linux Wayland will join it through the portal's VIRTUAL source.
 *
 * ── The mode ────────────────────────────────────────────────────────────────
 *
 * 1920×1080 at 120 Hz, SDR, on every platform: 120 Hz because the capture
 * cadence is what bounds the latency, 1080p because every encoder on every
 * bench carries it at that rate.
 *
 * A client that asks for "Match my screen" gets its own size instead, made on
 * demand: the size travels with the Activate request, the driver's mode list
 * is rewritten with it first (the file is ours — see settingsXml()), and the
 * driver reads that list when the device starts, which is exactly what
 * turning the display on does. A phone held in the hand therefore streams a
 * 2532×1170 desktop that no monitor on the market has. An unknown or
 * out-of-bounds size, or a machine whose settings file belongs to someone
 * else's VDD, quietly keeps 1080p — the mode stage is never fatal.
 *
 * Everything in this header that has no OS dependency (the request parser,
 * the settings XML, the names) is pure and covered by
 * tests/test_virtual_display.cpp on every platform.
 */
namespace VirtualDisplay {

// ── Names ───────────────────────────────────────────────────────────────────

/// What the display is called everywhere: the app card, the device node's
/// friendly name in Device Manager, the macOS descriptor. Per edition, so a
/// DEV install beside a PROD one has a node of its own: "MoonlightWeb Virtual
/// Display", "MoonlightWebDev Virtual Display".
QString displayNameFor(const QString& productName);
QString displayName();

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

// ── The mode ────────────────────────────────────────────────────────────────

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kRefreshHz = 120;

/// What a requested mode may be. The floor is a size a desktop is still
/// usable at, the ceiling is what the driver and every encoder on the bench
/// carry; odd sizes are rounded down because a display mode is made of whole
/// macroblocks on the encoder that follows it.
constexpr int kModeMin = 640;
constexpr int kModeMax = 4096;

/// The mode a request asks for, pinned into bounds and made even. Returns
/// false — and leaves 0×0 — when there is nothing usable to ask for, which is
/// the normal case: the default mode then applies.
bool normaliseMode(int& width, int& height);

// ── The bundled driver (Windows x64) ───────────────────────────────────────
//
// VirtualDrivers/Virtual-Display-Driver release 25.7.23, the bare driver
// package (inf + cat + dll), as committed under backend/installer/drivers/vdd
// and installed to {app}\drivers\vdd. Bumping the version means recomputing
// every hash below from the published file; a file that does not match is
// never handed to SetupAPI.

/// One file of the package as it must be found next to the exe.
struct DriverFile
{
    const char* name;   ///< file name under bundledDriverDir()
    const char* sha256; ///< lowercase hex
};

/// The three files SetupAPI consumes.
inline const std::array<DriverFile, 3>& driverFiles()
{
    static const std::array<DriverFile, 3> files = {{
        {"MttVDD.inf", "550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d"},
        {"mttvdd.cat", "08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a918dc859468f6"},
        {"MttVDD.dll", "c9ca837f57a98fbd43bc416a7f535a95843626e7759eaf85cf0cd7ce334dbb05"},
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
/// Shared by every instance of the driver on the machine — which is why a
/// file already there (the owner's own VDD) is left exactly as it is.
inline QString settingsXmlPath()
{
    return QStringLiteral("C:/VirtualDisplayDriver/vdd_settings.xml");
}

/// The driver files as the installer laid them out: {app}\drivers\vdd. Derived
/// from the exe's own location, never from a request.
QString bundledDriverDir();

// ── Request / result files ──────────────────────────────────────────────────

struct Request
{
    enum class Action
    {
        Install,    ///< installer: create our node, disabled (no-op if there)
        Uninstall,  ///< uninstaller: remove our node, the package if unused
        Activate,   ///< a stream starts: enable, set the mode, make primary
        Deactivate, ///< the last stream ended: restore the primary, disable
    };
    Action action = Action::Activate;
    /// Deactivate: the display to make primary again, as Activate reported
    /// it (Result::previousPrimary). Empty: leave the choice to the OS.
    QString restorePrimary;
    /// Activate: the size the client asked the display to have ("Match my
    /// screen"). 0×0 — the usual case — is the default mode. Always within
    /// [kModeMin, kModeMax] and even once parsed.
    int width = 0;
    int height = 0;
};

/// Parse and validate a request. The file is read by an elevated process, so
/// its contents are a choice among four verbs and one display key, never
/// free-form input. Returns nullopt with @p error filled on any deviation.
std::optional<Request> parseRequest(const QByteArray& json, QString* error);
QByteArray toJson(const Request& req);
QString toString(Request::Action action);

struct Result
{
    bool ok = false;
    QString stage; ///< where it ended: "snapshot" | "driver" | "mode" | "done"
    QString error; ///< English, for the log and the dialog
    bool rebootRequired = false;
    QString display; ///< GDI name of the display that came up ("\\.\DISPLAY3")
    /// Activate: the display that was primary before ours took the role — the
    /// monitor device path on Windows, the CG display id on macOS. Handed
    /// back on Deactivate as Request::restorePrimary.
    QString previousPrimary;
};

/// Tolerant of an empty or partial file: the server polls while the helper is
/// still writing. Returns nullopt until a complete object is there.
std::optional<Result> parseResult(const QByteArray& json);
QByteArray toJson(const Result& res);

/// The driver's vdd_settings.xml: a single monitor, our mode first (the
/// driver picks the first as default), the common sizes after it, SDR. A
/// @p width × @p height of 0 is the default 1920×1080; anything else is the
/// client's own size, added ahead of the list.
///
/// Written when none exists, and rewritten — with the node restarted so the
/// driver re-reads it — when the size changes. Never over a file this project
/// did not write: an owner's own VDD configured that one (see isOurSettings).
QString settingsXml(int width = 0, int height = 0);

/// Does this vdd_settings.xml carry our marker, i.e. did we write it? A file
/// that does not is left exactly as it is, mode on demand or not.
bool isOurSettings(const QString& xml);

// ── Paths ───────────────────────────────────────────────────────────────────

/// Per-user staging directory. Not %TEMP%: it holds files an elevated process
/// will act on, and C:\Windows\Temp is writable by every local account.
QString stagingDir();
QString requestPath();
QString resultPath();

// ── Status ──────────────────────────────────────────────────────────────────

struct Status
{
    bool supported = false; ///< this platform can have our virtual display
    bool installed = false; ///< Windows: our device node exists; macOS: always
    bool enabled = false;   ///< Windows: the node is enabled; macOS: created
    bool active = false;    ///< our display is in the probe's list right now
    bool canManage = false; ///< an elevation path exists (see method)
    QString method;         ///< "task" | "elevated" | "inprocess" (macOS) | ""
};

/// Probe everything. Cheap except for the Task Scheduler lookup, which is
/// cached for a while — nothing there changes without an install.
Status probe();

/// Serialise for the REST API and the host card.
QJsonObject toJson(const Status& st);

/// Is this display, as the engine enumerated it, OUR virtual display? Windows:
/// the monitor hanging off our device node; macOS: the display this process
/// created. Never true for another virtual display (the owner's own VDD, a
/// dummy plug), which stays an ordinary "Display N".
bool isOurs(const mw::native::DisplayInfo& display);

/// Windows: does our device node exist right now? (disabled counts)
bool nodePresent();

/// Windows: is our device node enabled?
bool nodeEnabled();

/// Windows: is the process token elevated?
bool processElevated();

// ── The in-process display (macOS) ─────────────────────────────────────────
//
// On macOS the display is not a driver but an object this process holds
// (mw::native::vdisplay, CoreGraphics' virtual display): no elevation, no
// helper — Status::method is "inprocess" and the job applies the request
// right here.

/// Activate (create + make main) or Deactivate (restore main + release).
/// Synchronous; the display may still be coming online when this returns
/// (the job polls).
bool applyInProcess(const Request& req, Result* result);

/// At startup: put the display back in its off state if the previous process
/// left it on (killed mid-stream). Windows: disable the node and restore the
/// remembered primary. macOS: nothing to do — the display died with the
/// process — beyond forgetting the record.
void resetAtStartup();

} // namespace VirtualDisplay
