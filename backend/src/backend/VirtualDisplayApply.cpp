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

#include "backend/VirtualDisplayApply.h"

#include "backend/VirtualDisplay.h"
#include "common/Logger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <cstdio>
#include <optional>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <wincrypt.h>
#include <vector>
#endif

// The helper behind `--vdisplay-apply`: four verbs on "MoonlightWeb Virtual
// Display" (VirtualDisplay.h), read from a request file, answered in a result
// file and on stdout.
//
//   install     installer, elevated — create our device node from the bundled
//               driver, named, and leave it DISABLED. No-op when it exists.
//   uninstall   uninstaller, elevated — remove our node; the driver package
//               and its settings only if no other node uses them.
//   activate    a stream starts — enable the node ("driver" stage, elevated),
//               then on the desktop ("mode" stage): our mode, SDR, and make
//               it the primary display, remembering which one was.
//   deactivate  the last stream ended — give the primary role back ("mode"),
//               then disable the node ("driver").
//
// The two stages exist because a service runs the elevated half as SYSTEM in
// session 0, where there is no desktop to set a mode on, and the desktop half
// in the console session as the user. The installer's task runs both at once
// (`--stage=all`), in the order the verb needs.

namespace VirtualDisplayApply {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitReboot = 3010; // ERROR_SUCCESS_REBOOT_REQUIRED, like a driver installer

struct Context
{
    QString dir;
    VirtualDisplay::Request request;
    VirtualDisplay::Result result;
};

/// The verdict goes to the result file (the task path has no stdout to read)
/// AND to stdout (the child paths read that). Both, always: the two readers
/// never know which way they were reached.
int finish(Context& ctx)
{
    const QByteArray json = VirtualDisplay::toJson(ctx.result);
    QFile f(ctx.dir + QStringLiteral("/result.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(json);
        f.close();
    } else {
        Logger::warning(QStringLiteral("[vdisplay-apply] cannot write %1").arg(f.fileName()));
    }
    std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
    std::fflush(stdout);
    if (ctx.result.ok)
        Logger::info(QStringLiteral("[vdisplay-apply] ok (%1)").arg(ctx.result.stage));
    else
        Logger::warning(QStringLiteral("[vdisplay-apply] failed at %1: %2")
                            .arg(ctx.result.stage, ctx.result.error));
    if (!ctx.result.ok) return kExitFailed;
    return ctx.result.rebootRequired ? kExitReboot : kExitOk;
}

int failAt(Context& ctx, const QString& stage, const QString& error)
{
    ctx.result.ok = false;
    ctx.result.stage = stage;
    ctx.result.error = error;
    return finish(ctx);
}

#ifdef Q_OS_WIN

QString lastErrorText(DWORD err = GetLastError())
{
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString text = n ? QString::fromWCharArray(buf, int(n)).trimmed() : QString();
    if (buf) LocalFree(buf);
    return QStringLiteral("0x%1 %2").arg(err, 8, 16, QLatin1Char('0')).arg(text);
}

QString fileSha256(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) return QString();
    return QString::fromLatin1(h.result().toHex());
}

// ── Trusted Publishers, for the duration of the install ────────────────────
//
// A user-mode driver package installs without a prompt only when the
// catalog's signer is a trusted publisher. The signer is pinned by thumbprint
// (VirtualDisplay::catalogSignerThumbprint); the certificate itself is taken
// from inside the catalog we just hashed, never from anywhere else, added to
// the machine store for the install, and deleted again unless it was already
// there before us. Nothing signed by anyone else is ever trusted, and the
// trust does not outlive this process.

struct PublisherTrust
{
    HCERTSTORE store = nullptr;
    PCCERT_CONTEXT added = nullptr; // the copy in the store, when WE added it
    bool preexisting = false;

    ~PublisherTrust()
    {
        if (added) {
            // CertDeleteCertificateFromStore frees the context either way.
            if (!CertDeleteCertificateFromStore(added))
                Logger::warning(
                    QStringLiteral("[vdisplay-apply] could not untrust the publisher: %1")
                        .arg(lastErrorText()));
            else
                Logger::info(QStringLiteral("[vdisplay-apply] publisher untrusted again"));
            added = nullptr;
        }
        if (store) CertCloseStore(store, 0);
    }
};

bool trustCatalogSigner(const QString& catPath, PublisherTrust& trust, QString* error)
{
    const QByteArray wanted =
        QByteArray::fromHex(VirtualDisplay::catalogSignerThumbprint().toLatin1());

    HCERTSTORE catStore = nullptr;
    HCRYPTMSG msg = nullptr;
    const std::wstring wpath = QDir::toNativeSeparators(catPath).toStdWString();
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, wpath.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED | CERT_QUERY_CONTENT_FLAG_CERT,
                          CERT_QUERY_FORMAT_FLAG_ALL, 0, nullptr, nullptr, nullptr, &catStore, &msg,
                          nullptr)) {
        *error = QStringLiteral("cannot read the catalog's signature: %1").arg(lastErrorText());
        return false;
    }
    if (msg) CryptMsgClose(msg);

    PCCERT_CONTEXT match = nullptr;
    for (PCCERT_CONTEXT c = CertEnumCertificatesInStore(catStore, nullptr); c;
         c = CertEnumCertificatesInStore(catStore, c)) {
        BYTE hash[20];
        DWORD len = sizeof(hash);
        if (!CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, hash, &len)) continue;
        if (QByteArray(reinterpret_cast<const char*>(hash), int(len)) == wanted) {
            match = CertDuplicateCertificateContext(c);
            CertFreeCertificateContext(c);
            break;
        }
    }
    CertCloseStore(catStore, 0);
    if (!match) {
        *error = QStringLiteral("the catalog is not signed by the pinned publisher");
        return false;
    }

    trust.store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
                                L"TrustedPublisher");
    if (!trust.store)
        trust.store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE,
                                    L"TrustedPublisher");
    if (!trust.store) {
        CertFreeCertificateContext(match);
        *error =
            QStringLiteral("cannot open the Trusted Publishers store: %1").arg(lastErrorText());
        return false;
    }

    CRYPT_HASH_BLOB blob{DWORD(wanted.size()),
                         reinterpret_cast<BYTE*>(const_cast<char*>(wanted.constData()))};
    if (PCCERT_CONTEXT existing =
            CertFindCertificateInStore(trust.store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                       CERT_FIND_SHA1_HASH, &blob, nullptr)) {
        // Already trusted on this machine (the driver's own installer does the
        // same thing, permanently): not ours to remove afterwards.
        CertFreeCertificateContext(existing);
        CertFreeCertificateContext(match);
        trust.preexisting = true;
        Logger::info(QStringLiteral("[vdisplay-apply] publisher already trusted"));
        return true;
    }
    const BOOL ok =
        CertAddCertificateContextToStore(trust.store, match, CERT_STORE_ADD_NEW, &trust.added);
    CertFreeCertificateContext(match);
    if (!ok) {
        *error = QStringLiteral("cannot trust the publisher: %1").arg(lastErrorText());
        return false;
    }
    Logger::info(QStringLiteral("[vdisplay-apply] publisher trusted for this install"));
    return true;
}

// ── Device nodes ────────────────────────────────────────────────────────────

/// A device info set holding every Display-class node, present or not, with
/// a way to find ours (hardware id + friendly name) among them.
struct DisplayNodes
{
    HDEVINFO set = INVALID_HANDLE_VALUE;

    DisplayNodes()
        : set(SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0))
    {}
    ~DisplayNodes()
    {
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    }
    bool valid() const { return set != INVALID_HANDLE_VALUE; }

    QString property(const SP_DEVINFO_DATA& data, DWORD prop) const
    {
        DWORD size = 0;
        SetupDiGetDeviceRegistryPropertyW(set, const_cast<SP_DEVINFO_DATA*>(&data), prop, nullptr,
                                          nullptr, 0, &size);
        if (size == 0) return QString();
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
        if (!SetupDiGetDeviceRegistryPropertyW(set, const_cast<SP_DEVINFO_DATA*>(&data), prop,
                                               nullptr, reinterpret_cast<BYTE*>(buf.data()), size,
                                               nullptr))
            return QString();
        return QString::fromWCharArray(buf.data()); // the first string of a MULTI_SZ
    }

    bool hasHardwareId(const SP_DEVINFO_DATA& data, const QString& wanted) const
    {
        DWORD size = 0;
        SetupDiGetDeviceRegistryPropertyW(set, const_cast<SP_DEVINFO_DATA*>(&data),
                                          SPDRP_HARDWAREID, nullptr, nullptr, 0, &size);
        if (size == 0) return false;
        std::vector<wchar_t> ids(size / sizeof(wchar_t) + 1, 0);
        if (!SetupDiGetDeviceRegistryPropertyW(set, const_cast<SP_DEVINFO_DATA*>(&data),
                                               SPDRP_HARDWAREID, nullptr,
                                               reinterpret_cast<BYTE*>(ids.data()), size, nullptr))
            return false;
        for (const wchar_t* h = ids.data(); *h; h += wcslen(h) + 1)
            if (QString::fromWCharArray(h).compare(wanted, Qt::CaseInsensitive) == 0) return true;
        return false;
    }

    /// Every node with the driver's hardware id; `ours` set on the one that
    /// carries our friendly name.
    struct Node
    {
        SP_DEVINFO_DATA data{};
        bool ours = false;
    };
    std::vector<Node> driverNodes() const
    {
        std::vector<Node> out;
        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof(data);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i) {
            if (!hasHardwareId(data, VirtualDisplay::hardwareId())) continue;
            Node n;
            n.data = data;
            n.ours = property(data, SPDRP_FRIENDLYNAME)
                         .compare(VirtualDisplay::displayName(), Qt::CaseInsensitive) == 0;
            out.push_back(n);
        }
        return out;
    }

    std::optional<SP_DEVINFO_DATA> ours() const
    {
        for (const Node& n : driverNodes())
            if (n.ours) return n.data;
        return std::nullopt;
    }
};

bool setEnabled(HDEVINFO set, SP_DEVINFO_DATA& data, bool enable, QString* error)
{
    // What Device Manager's Enable/Disable does: a property change, global
    // then for the current hardware profile (the second is what makes it
    // take effect at once; devcon does the same pair).
    for (const DWORD scope : {DWORD(DICS_FLAG_GLOBAL), DWORD(DICS_FLAG_CONFIGSPECIFIC)}) {
        SP_PROPCHANGE_PARAMS params{};
        params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        params.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
        params.Scope = scope;
        params.HwProfile = 0;
        if (!SetupDiSetClassInstallParamsW(set, &data, &params.ClassInstallHeader,
                                           sizeof(params)) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &data)) {
            if (scope == DICS_FLAG_GLOBAL) {
                *error = QStringLiteral("%1 the device: %2")
                             .arg(enable ? QStringLiteral("enabling") : QStringLiteral("disabling"))
                             .arg(lastErrorText());
                return false;
            }
        }
    }
    Logger::info(QStringLiteral("[vdisplay-apply] device node %1")
                     .arg(enable ? QStringLiteral("enabled") : QStringLiteral("disabled")));
    return true;
}

bool nodeIsEnabled(SP_DEVINFO_DATA& data)
{
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) != CR_SUCCESS) return false;
    return !((status & DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED);
}

/// The name that makes a node ours — in Device Manager and to every lookup.
bool setFriendlyName(HDEVINFO set, SP_DEVINFO_DATA& data, QString* error)
{
    const std::wstring name = VirtualDisplay::displayName().toStdWString();
    if (!SetupDiSetDeviceRegistryPropertyW(set, &data, SPDRP_FRIENDLYNAME,
                                           reinterpret_cast<const BYTE*>(name.c_str()),
                                           DWORD((name.size() + 1) * sizeof(wchar_t)))) {
        *error = QStringLiteral("SetupDiSetDeviceRegistryProperty(friendly name): %1")
                     .arg(lastErrorText());
        return false;
    }
    return true;
}

/// Create our node: a root-enumerated Display-class device with the driver's
/// hardware id and our friendly name, registered but not yet driven.
bool createOurNode(HDEVINFO set, SP_DEVINFO_DATA& data, QString* error)
{
    data = SP_DEVINFO_DATA{};
    data.cbSize = sizeof(data);
    if (!SetupDiCreateDeviceInfoW(set, L"Display", &GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                  DICD_GENERATE_ID, &data)) {
        *error = QStringLiteral("SetupDiCreateDeviceInfo: %1").arg(lastErrorText());
        return false;
    }
    // A REG_MULTI_SZ: the id, then two terminators.
    std::wstring hw = VirtualDisplay::hardwareId().toStdWString();
    std::vector<wchar_t> multi(hw.begin(), hw.end());
    multi.push_back(0);
    multi.push_back(0);
    if (!SetupDiSetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID,
                                           reinterpret_cast<const BYTE*>(multi.data()),
                                           DWORD(multi.size() * sizeof(wchar_t)))) {
        *error = QStringLiteral("SetupDiSetDeviceRegistryProperty(hardware id): %1")
                     .arg(lastErrorText());
        return false;
    }
    if (!setFriendlyName(set, data, error)) return false;
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &data)) {
        *error = QStringLiteral("DIF_REGISTERDEVICE: %1").arg(lastErrorText());
        return false;
    }
    return true;
}

bool removeNode(HDEVINFO set, SP_DEVINFO_DATA& data, QString* error)
{
    SP_REMOVEDEVICE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    params.Scope = DI_REMOVEDEVICE_GLOBAL;
    if (!SetupDiSetClassInstallParamsW(set, &data, &params.ClassInstallHeader, sizeof(params)) ||
        !SetupDiCallClassInstaller(DIF_REMOVE, set, &data)) {
        *error = QStringLiteral("DIF_REMOVE: %1").arg(lastErrorText());
        return false;
    }
    Logger::info(QStringLiteral("[vdisplay-apply] device node removed"));
    return true;
}

/// Stage the package in the driver store and install it on OUR node only —
/// never on another node with the same hardware id (the owner's own VDD
/// keeps whatever driver it has).
bool installOnNode(HDEVINFO set, SP_DEVINFO_DATA& data, const QString& infPath, bool* reboot,
                   QString* error)
{
    const std::wstring inf = QDir::toNativeSeparators(infPath).toStdWString();
    if (!SetupCopyOEMInfW(inf.c_str(), nullptr, SPOST_PATH, 0, nullptr, 0, nullptr, nullptr)) {
        *error = QStringLiteral("SetupCopyOEMInf: %1").arg(lastErrorText());
        return false;
    }
    BOOL needReboot = FALSE;
    // With no driver named, DiInstallDevice picks the best-ranked package in
    // the store for this node — the one just staged, or an identical or newer
    // copy of the same driver the owner installed before us.
    if (!DiInstallDevice(nullptr, set, &data, nullptr, 0, &needReboot)) {
        *error = QStringLiteral("DiInstallDevice: %1").arg(lastErrorText());
        return false;
    }
    *reboot = needReboot != FALSE;
    return true;
}

/// Delete the package from the driver store: find the oemNN.inf whose provider
/// is the driver's, then SetupUninstallOEMInf. Best effort — a package left in
/// the store is harmless once its nodes are gone.
void deletePackage()
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
    if (set == INVALID_HANDLE_VALUE) return;
    if (SetupDiBuildDriverInfoList(set, nullptr, SPDIT_CLASSDRIVER)) {
        SP_DRVINFO_DATA_W drv{};
        drv.cbSize = sizeof(drv);
        for (DWORD i = 0; SetupDiEnumDriverInfoW(set, nullptr, SPDIT_CLASSDRIVER, i, &drv); ++i) {
            if (QString::fromWCharArray(drv.ProviderName)
                    .compare(QStringLiteral("MikeTheTech"), Qt::CaseInsensitive) != 0)
                continue;
            DWORD need = 0;
            SetupDiGetDriverInfoDetailW(set, nullptr, &drv, nullptr, 0, &need);
            if (need == 0) continue;
            std::vector<BYTE> buf(need);
            auto* detail = reinterpret_cast<PSP_DRVINFO_DETAIL_DATA_W>(buf.data());
            detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
            if (!SetupDiGetDriverInfoDetailW(set, nullptr, &drv, detail, need, nullptr)) continue;
            const QString infName =
                QFileInfo(QString::fromWCharArray(detail->InfFileName)).fileName();
            if (!infName.startsWith(QLatin1String("oem"), Qt::CaseInsensitive)) continue;
            const std::wstring w = infName.toStdWString();
            if (SetupUninstallOEMInfW(w.c_str(), SUOI_FORCEDELETE, nullptr))
                Logger::info(
                    QStringLiteral("[vdisplay-apply] driver package %1 deleted").arg(infName));
            else
                Logger::warning(QStringLiteral("[vdisplay-apply] SetupUninstallOEMInf(%1): %2")
                                    .arg(infName, lastErrorText()));
        }
        SetupDiDestroyDriverInfoList(set, nullptr, SPDIT_CLASSDRIVER);
    }
    SetupDiDestroyDeviceInfoList(set);
}

// ── Stage "driver": the elevated half ───────────────────────────────────────

int stageDriver(Context& ctx)
{
    using Action = VirtualDisplay::Request::Action;
    const QString stage = QStringLiteral("driver");
    if (!VirtualDisplay::processElevated())
        return failAt(ctx, stage, QStringLiteral("this process is not elevated"));

    DisplayNodes nodes;
    if (!nodes.valid())
        return failAt(ctx, stage, QStringLiteral("SetupDiGetClassDevs: %1").arg(lastErrorText()));
    QString error;

    switch (ctx.request.action) {
    case Action::Install: {
        if (nodes.ours()) {
            Logger::info(QStringLiteral("[vdisplay-apply] \"%1\" is already installed")
                             .arg(VirtualDisplay::displayName()));
            ctx.result.ok = true;
            ctx.result.stage = QStringLiteral("done");
            return finish(ctx);
        }
        // The files as the installer laid them out, re-hashed against the
        // constants compiled into THIS exe before SetupAPI sees them.
        const QString dir = VirtualDisplay::bundledDriverDir();
        QString infPath, catPath;
        for (const VirtualDisplay::DriverFile& f : VirtualDisplay::driverFiles()) {
            const QString path = dir + QLatin1Char('/') + QLatin1String(f.name);
            if (fileSha256(path) != QLatin1String(f.sha256))
                return failAt(ctx, stage,
                              QStringLiteral("%1 is missing or does not match its pinned hash")
                                  .arg(QLatin1String(f.name)));
            if (path.endsWith(QLatin1String(".inf"), Qt::CaseInsensitive)) infPath = path;
            if (path.endsWith(QLatin1String(".cat"), Qt::CaseInsensitive)) catPath = path;
        }
        // The driver's settings: only when nobody wrote any. An owner's own
        // VDD configured that file, and ours takes the modes it lists.
        const QString xmlPath = VirtualDisplay::settingsXmlPath();
        if (!QFile::exists(xmlPath)) {
            if (!QDir().mkpath(QFileInfo(xmlPath).absolutePath()))
                return failAt(ctx, stage, QStringLiteral("cannot create %1").arg(xmlPath));
            QFile f(xmlPath);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return failAt(ctx, stage, QStringLiteral("cannot write %1").arg(xmlPath));
            f.write(VirtualDisplay::settingsXml().toUtf8());
        } else {
            Logger::info(QStringLiteral("[vdisplay-apply] %1 exists — left as it is").arg(xmlPath));
        }

        PublisherTrust trust;
        if (!trustCatalogSigner(catPath, trust, &error)) return failAt(ctx, stage, error);
        SP_DEVINFO_DATA data{};
        if (!createOurNode(nodes.set, data, &error)) return failAt(ctx, stage, error);
        bool reboot = false;
        if (!installOnNode(nodes.set, data, infPath, &reboot, &error)) {
            // Leave no orphan node behind a failed install.
            QString ignored;
            removeNode(nodes.set, data, &ignored);
            return failAt(ctx, stage, error);
        }
        // The name again, now that the driver is bound: installing the
        // package rewrites the node's description from the INF and, as
        // measured on the bench, drops the friendly name set before it. This
        // write is the one that lasts — and the one every lookup relies on.
        if (!setFriendlyName(nodes.set, data, &error)) {
            QString ignored;
            removeNode(nodes.set, data, &ignored);
            return failAt(ctx, stage, error);
        }
        // Off by default: the card turns it on.
        if (!setEnabled(nodes.set, data, false, &error))
            Logger::warning(QStringLiteral("[vdisplay-apply] %1").arg(error));
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        ctx.result.rebootRequired = reboot;
        Logger::info(QStringLiteral("[vdisplay-apply] \"%1\" installed%2")
                         .arg(VirtualDisplay::displayName(),
                              reboot ? QStringLiteral(" (reboot required)") : QString()));
        return finish(ctx);
    }

    case Action::Uninstall: {
        bool othersRemain = false;
        bool ok = true;
        for (DisplayNodes::Node n : nodes.driverNodes()) {
            if (!n.ours) {
                othersRemain = true;
                continue;
            }
            if (!removeNode(nodes.set, n.data, &error)) ok = false;
        }
        if (!ok) return failAt(ctx, stage, error);
        if (!othersRemain) {
            deletePackage();
            QFile::remove(VirtualDisplay::settingsXmlPath());
        } else {
            Logger::info(QStringLiteral(
                "[vdisplay-apply] another node uses the driver — package and settings kept"));
        }
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        return finish(ctx);
    }

    case Action::Activate:
    case Action::Deactivate: {
        auto ours = nodes.ours();
        if (!ours)
            return failAt(ctx, stage,
                          QStringLiteral("\"%1\" is not installed on this machine")
                              .arg(VirtualDisplay::displayName()));
        const bool enable = ctx.request.action == Action::Activate;
        if (nodeIsEnabled(*ours) == enable) {
            Logger::info(QStringLiteral("[vdisplay-apply] device node already %1")
                             .arg(enable ? QStringLiteral("enabled") : QStringLiteral("disabled")));
        } else if (!setEnabled(nodes.set, *ours, enable, &error)) {
            return failAt(ctx, stage, error);
        }
        ctx.result.ok = true;
        ctx.result.stage = stage;
        return kExitOk; // the caller decides whether a mode stage follows
    }
    }
    return failAt(ctx, stage, QStringLiteral("unknown action"));
}

// ── Stage "mode": the desktop half ──────────────────────────────────────────

struct ActivePath
{
    LUID adapterId{};
    UINT32 targetId = 0;
    QString gdiName;    // "\\.\DISPLAY3"
    QString devicePath; // "\\?\DISPLAY#MTT1337#1&15ecd195&0&UID256#{…}"
    bool primary = false;
};

/// Every active display path, with what identifies it on both sides: the
/// GDI name (mode calls) and the monitor device path (stable identity).
std::vector<ActivePath> activePaths()
{
    std::vector<ActivePath> out;
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS)
        return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return out;

    for (UINT32 i = 0; i < nPaths; ++i) {
        const DISPLAYCONFIG_PATH_INFO& p = paths[i];
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;

        ActivePath a;
        a.adapterId = p.targetInfo.adapterId;
        a.targetId = p.targetInfo.id;
        a.gdiName = QString::fromWCharArray(sn.viewGdiDeviceName);
        a.devicePath = QString::fromWCharArray(tn.monitorDevicePath);
        const UINT32 srcIdx = p.sourceInfo.modeInfoIdx;
        if (srcIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && srcIdx < modes.size() &&
            modes[srcIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const POINTL& pos = modes[srcIdx].sourceMode.position;
            a.primary = pos.x == 0 && pos.y == 0;
        }
        out.push_back(a);
    }
    return out;
}

/// The monitor ids hanging off our node, spelled like a device path.
QStringList ourMonitorIds()
{
    QStringList out;
    DisplayNodes nodes;
    if (!nodes.valid()) return out;
    const auto ours = nodes.ours();
    if (!ours) return out;
    DEVINST child = 0;
    if (CM_Get_Child(&child, ours->DevInst, 0) != CR_SUCCESS) return out;
    for (;;) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(child, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
            QString s = QString::fromWCharArray(id).toLower();
            s.replace(QLatin1Char('\\'), QLatin1Char('#'));
            out.append(s);
        }
        DEVINST sibling = 0;
        if (CM_Get_Sibling(&sibling, child, 0) != CR_SUCCESS) break;
        child = sibling;
    }
    return out;
}

std::optional<ActivePath> findOurTarget()
{
    const QStringList ids = ourMonitorIds();
    if (ids.isEmpty()) return std::nullopt;
    for (const ActivePath& p : activePaths()) {
        const QString path = p.devicePath.toLower();
        for (const QString& id : ids)
            if (path.contains(id)) return p;
    }
    return std::nullopt;
}

/// Query the active display configuration, let @p edit change it, apply it.
/// The CCD API (what Windows Settings uses), not ChangeDisplaySettingsEx:
/// on the bench an indirect display just brought up took neither a mode nor
/// a position through GDI — success returned, nothing changed.
template <typename Edit> bool applyDisplayConfig(Edit edit, QString* error)
{
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) {
        *error = QStringLiteral("GetDisplayConfigBufferSizes failed");
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        *error = QStringLiteral("QueryDisplayConfig failed");
        return false;
    }
    paths.resize(nPaths);
    modes.resize(nModes);
    if (!edit(paths, modes, error)) return false;
    const LONG rc = SetDisplayConfig(nPaths, paths.data(), nModes, modes.data(),
                                     SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                                         SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    if (rc != ERROR_SUCCESS) {
        *error = QStringLiteral("SetDisplayConfig: %1").arg(lastErrorText(DWORD(rc)));
        return false;
    }
    return true;
}

/// The index of the active path whose monitor device path is @p devicePath.
int pathIndexFor(const std::vector<DISPLAYCONFIG_PATH_INFO>& paths, const QString& devicePath)
{
    for (size_t i = 0; i < paths.size(); ++i) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = paths[i].targetInfo.adapterId;
        tn.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
        if (QString::fromWCharArray(tn.monitorDevicePath)
                .compare(devicePath, Qt::CaseInsensitive) == 0)
            return int(i);
    }
    return -1;
}

/// Our mode on the display at @p devicePath: the source (desktop) size, and
/// the target left to the driver at the wanted refresh.
bool setMode(const QString& devicePath, QString* error)
{
    return applyDisplayConfig(
        [&](std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
            std::vector<DISPLAYCONFIG_MODE_INFO>& modes, QString* err) {
            const int i = pathIndexFor(paths, devicePath);
            if (i < 0) {
                *err = QStringLiteral("the display is not in the active configuration");
                return false;
            }
            DISPLAYCONFIG_PATH_INFO& p = paths[size_t(i)];
            const UINT32 src = p.sourceInfo.modeInfoIdx;
            if (src == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || src >= modes.size()) {
                *err = QStringLiteral("the display has no source mode");
                return false;
            }
            modes[src].sourceMode.width = UINT32(VirtualDisplay::kWidth);
            modes[src].sourceMode.height = UINT32(VirtualDisplay::kHeight);
            p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            p.targetInfo.refreshRate.Numerator = UINT32(VirtualDisplay::kRefreshHz);
            p.targetInfo.refreshRate.Denominator = 1;
            p.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
            p.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
            return true;
        },
        error);
}

/// Make the display at @p devicePath the primary one: every desktop origin
/// shifted so that one lands on (0,0), in one configuration change.
bool makePrimary(const QString& devicePath, QString* error)
{
    return applyDisplayConfig(
        [&](std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
            std::vector<DISPLAYCONFIG_MODE_INFO>& modes, QString* err) {
            const int i = pathIndexFor(paths, devicePath);
            if (i < 0) {
                *err = QStringLiteral("the display is not in the active configuration");
                return false;
            }
            const UINT32 src = paths[size_t(i)].sourceInfo.modeInfoIdx;
            if (src == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || src >= modes.size()) {
                *err = QStringLiteral("the display has no source mode");
                return false;
            }
            const POINTL origin = modes[src].sourceMode.position;
            if (origin.x == 0 && origin.y == 0) return true; // already the primary
            for (DISPLAYCONFIG_MODE_INFO& m : modes) {
                if (m.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
                m.sourceMode.position.x -= origin.x;
                m.sourceMode.position.y -= origin.y;
            }
            return true;
        },
        error);
}

int stageMode(Context& ctx)
{
    using Action = VirtualDisplay::Request::Action;
    const QString stage = QStringLiteral("mode");

    if (ctx.request.action == Action::Deactivate) {
        // The previous primary back in its role, while ours still exists —
        // Windows would otherwise pick one itself when ours goes.
        if (!ctx.request.restorePrimary.isEmpty()) {
            bool found = false;
            for (const ActivePath& p : activePaths()) {
                if (p.devicePath.compare(ctx.request.restorePrimary, Qt::CaseInsensitive) != 0)
                    continue;
                found = true;
                QString error;
                if (!p.primary && !makePrimary(p.devicePath, &error))
                    Logger::warning(
                        QStringLiteral("[vdisplay-apply] previous primary not restored: %1")
                            .arg(error));
            }
            if (!found)
                Logger::info(QStringLiteral(
                    "[vdisplay-apply] the previous primary display is gone — Windows picks"));
        }
        ctx.result.ok = true;
        ctx.result.stage = stage;
        return kExitOk; // the driver stage (disable) follows
    }
    if (ctx.request.action != Action::Activate) {
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        return finish(ctx);
    }

    // The display appears a moment after the driver starts; ten seconds is
    // generous for a UMDF driver and a single monitor.
    std::optional<ActivePath> target;
    QElapsedTimer wait;
    wait.start();
    while (!(target = findOurTarget()) && wait.elapsed() < 10 * 1000)
        QThread::msleep(250);
    if (!target) {
        if (ctx.result.rebootRequired) {
            ctx.result.ok = true;
            ctx.result.stage = QStringLiteral("done");
            return finish(ctx);
        }
        return failAt(ctx, stage, QStringLiteral("the virtual display did not appear"));
    }
    ctx.result.display = target->gdiName;

    // Who holds the primary role right now, for the way back.
    for (const ActivePath& p : activePaths())
        if (p.primary && p.devicePath.compare(target->devicePath, Qt::CaseInsensitive) != 0)
            ctx.result.previousPrimary = p.devicePath;

    QString error;
    if (setMode(target->devicePath, &error)) {
        Logger::info(QStringLiteral("[vdisplay-apply] %1 set to %2x%3@%4")
                         .arg(target->gdiName)
                         .arg(VirtualDisplay::kWidth)
                         .arg(VirtualDisplay::kHeight)
                         .arg(VirtualDisplay::kRefreshHz));
    } else {
        // Not fatal: the display exists and streams at whatever mode it has.
        Logger::warning(QStringLiteral("[vdisplay-apply] mode %1x%2@%3 refused on %4: %5")
                            .arg(VirtualDisplay::kWidth)
                            .arg(VirtualDisplay::kHeight)
                            .arg(VirtualDisplay::kRefreshHz)
                            .arg(target->gdiName, error));
    }

    // SDR: the same call scripts/Set-DisplayHdr.ps1 makes. The target ids may
    // have moved with the mode change, so look the display up again.
    if (auto again = findOurTarget()) target = again;
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE ac{};
    ac.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    ac.header.size = sizeof(ac);
    ac.header.adapterId = target->adapterId;
    ac.header.id = target->targetId;
    ac.enableAdvancedColor = 0;
    const LONG hr = DisplayConfigSetDeviceInfo(&ac.header);
    if (hr != ERROR_SUCCESS && hr != ERROR_NOT_SUPPORTED)
        Logger::warning(
            QStringLiteral("[vdisplay-apply] SDR refused: %1").arg(lastErrorText(DWORD(hr))));

    if (!makePrimary(target->devicePath, &error))
        Logger::warning(QStringLiteral("[vdisplay-apply] not made primary: %1").arg(error));

    ctx.result.ok = true;
    ctx.result.stage = QStringLiteral("done");
    return finish(ctx);
}

#endif // Q_OS_WIN

} // namespace

int run(const QString& stageArg, const QString& dirArg)
{
    Context ctx;
    ctx.dir = dirArg.isEmpty() ? VirtualDisplay::stagingDir() : QDir::fromNativeSeparators(dirArg);
    const QString stage = stageArg.isEmpty() ? QStringLiteral("all") : stageArg;

    QFile req(ctx.dir + QStringLiteral("/request.json"));
    if (!req.open(QIODevice::ReadOnly))
        return failAt(ctx, QStringLiteral("request"),
                      QStringLiteral("no request file at %1").arg(req.fileName()));
    QString error;
    const auto parsed = VirtualDisplay::parseRequest(req.readAll(), &error);
    if (!parsed) return failAt(ctx, QStringLiteral("request"), error);
    ctx.request = *parsed;

#ifdef Q_OS_WIN
    using Action = VirtualDisplay::Request::Action;
    const bool wantDriver = stage == QLatin1String("driver") || stage == QLatin1String("all");
    const bool wantMode = stage == QLatin1String("mode") || stage == QLatin1String("all");
    if (!wantDriver && !wantMode)
        return failAt(ctx, QStringLiteral("request"),
                      QStringLiteral("unknown stage %1").arg(stage));

    // Deactivate runs the desktop half first (restore the primary while our
    // display still exists), every other verb the elevated half first.
    const bool modeFirst = ctx.request.action == Action::Deactivate;
    const auto first =
        modeFirst ? (wantMode ? &stageMode : nullptr) : (wantDriver ? &stageDriver : nullptr);
    const auto second =
        modeFirst ? (wantDriver ? &stageDriver : nullptr) : (wantMode ? &stageMode : nullptr);

    if (first) {
        const int rc = first(ctx);
        // A stage finishes itself on failure and when the verb is complete;
        // otherwise it returns without writing so the other half can go on.
        if (!ctx.result.ok || ctx.result.stage == QLatin1String("done")) return rc;
        if (!second) return finish(ctx);
    }
    if (second) {
        const int rc = second(ctx);
        if (!ctx.result.ok || ctx.result.stage == QLatin1String("done")) return rc;
        // Deactivate ends on the driver stage, which does not finish itself.
        ctx.result.stage = QStringLiteral("done");
    }
    return finish(ctx);
#else
    Q_UNUSED(stage);
    return failAt(ctx, QStringLiteral("request"),
                  QStringLiteral("virtual displays are not supported on this platform"));
#endif
}

} // namespace VirtualDisplayApply
