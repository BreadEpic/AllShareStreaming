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
#include <wincrypt.h>
#include <vector>
#endif

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

// ── Device node + driver package ────────────────────────────────────────────

bool createDeviceNode(QString* error)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        *error = QStringLiteral("SetupDiCreateDeviceInfoList: %1").arg(lastErrorText());
        return false;
    }
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    bool ok = SetupDiCreateDeviceInfoW(set, L"Display", &GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                       DICD_GENERATE_ID, &data) != FALSE;
    if (!ok) {
        *error = QStringLiteral("SetupDiCreateDeviceInfo: %1").arg(lastErrorText());
    } else {
        // A REG_MULTI_SZ: the id, then two terminators.
        std::wstring hw = VirtualDisplay::hardwareId().toStdWString();
        std::vector<wchar_t> multi(hw.begin(), hw.end());
        multi.push_back(0);
        multi.push_back(0);
        ok = SetupDiSetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID,
                                               reinterpret_cast<const BYTE*>(multi.data()),
                                               DWORD(multi.size() * sizeof(wchar_t))) != FALSE;
        if (!ok)
            *error = QStringLiteral("SetupDiSetDeviceRegistryProperty: %1").arg(lastErrorText());
    }
    if (ok) {
        ok = SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &data) != FALSE;
        if (!ok) *error = QStringLiteral("DIF_REGISTERDEVICE: %1").arg(lastErrorText());
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

bool installPackage(const QString& infPath, bool* reboot, QString* error)
{
    const std::wstring inf = QDir::toNativeSeparators(infPath).toStdWString();
    const std::wstring hw = VirtualDisplay::hardwareId().toStdWString();
    BOOL needReboot = FALSE;
    // The devcon "install" second half: match the package to every node with
    // this hardware id and install it. INSTALLFLAG_FORCE: a Windows that thinks
    // it has a better driver for a device that has none is not a judgement call
    // worth honouring here.
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hw.c_str(), inf.c_str(), INSTALLFLAG_FORCE,
                                            &needReboot)) {
        *error = QStringLiteral("UpdateDriverForPlugAndPlayDevices: %1").arg(lastErrorText());
        return false;
    }
    *reboot = needReboot != FALSE;
    return true;
}

/// Remove every device node carrying the hardware id (SetupAPI, no pnputil:
/// its output is localised and its exit codes are not a contract).
bool removeDeviceNodes(QString* error)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) {
        *error = QStringLiteral("SetupDiGetClassDevs: %1").arg(lastErrorText());
        return false;
    }
    const QString wanted = VirtualDisplay::hardwareId();
    bool ok = true;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, nullptr, nullptr, 0, &size);
        if (size == 0) continue;
        std::vector<wchar_t> ids(size / sizeof(wchar_t) + 1, 0);
        if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, nullptr,
                                               reinterpret_cast<BYTE*>(ids.data()), size, nullptr))
            continue;
        bool match = false;
        for (const wchar_t* h = ids.data(); *h; h += wcslen(h) + 1)
            if (QString::fromWCharArray(h).compare(wanted, Qt::CaseInsensitive) == 0) match = true;
        if (!match) continue;

        SP_REMOVEDEVICE_PARAMS params{};
        params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        params.ClassInstallHeader.InstallFunction = DIF_REMOVE;
        params.Scope = DI_REMOVEDEVICE_GLOBAL;
        if (!SetupDiSetClassInstallParamsW(set, &data, &params.ClassInstallHeader,
                                           sizeof(params)) ||
            !SetupDiCallClassInstaller(DIF_REMOVE, set, &data)) {
            *error = QStringLiteral("DIF_REMOVE: %1").arg(lastErrorText());
            ok = false;
        } else {
            Logger::info(QStringLiteral("[vdisplay-apply] device node removed"));
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

/// Delete the package from the driver store: find the oemNN.inf whose provider
/// and description are the driver's, then SetupUninstallOEMInf. Best effort —
/// a package left in the store is harmless once its node is gone.
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

// ── Stage "driver" ──────────────────────────────────────────────────────────

int stageDriver(Context& ctx)
{
    const QString stage = QStringLiteral("driver");
    if (!VirtualDisplay::processElevated())
        return failAt(ctx, stage, QStringLiteral("this process is not elevated"));

    if (ctx.request.action == VirtualDisplay::Request::Action::Remove) {
        QString error;
        if (!removeDeviceNodes(&error)) return failAt(ctx, stage, error);
        deletePackage();
        QFile::remove(VirtualDisplay::settingsXmlPath());
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        return finish(ctx);
    }

    // The settings file first, whether or not the driver is already there: a
    // driver already installed by hand keeps its node and just gets our modes.
    const QString xmlPath = VirtualDisplay::settingsXmlPath();
    if (!QDir().mkpath(QFileInfo(xmlPath).absolutePath()))
        return failAt(ctx, stage, QStringLiteral("cannot create %1").arg(xmlPath));
    {
        QFile f(xmlPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return failAt(ctx, stage, QStringLiteral("cannot write %1").arg(xmlPath));
        f.write(VirtualDisplay::settingsXml(ctx.request).toUtf8());
    }

    if (VirtualDisplay::driverPresent()) {
        Logger::info(QStringLiteral("[vdisplay-apply] driver already installed — settings only"));
        ctx.result.ok = true;
        ctx.result.stage = stage;
        return kExitOk; // the caller continues with the mode stage
    }

    // Re-verify the staged files against the constants compiled into THIS exe:
    // the unprivileged server hashed them, but this is the process that acts
    // on them, and a re-hash costs nothing next to a driver install.
    const QString driverDir = ctx.dir + QStringLiteral("/driver");
    QString infPath, catPath;
    for (const VirtualDisplay::DriverFile& f : VirtualDisplay::driverFiles()) {
        const QString path = driverDir + QLatin1Char('/') + QLatin1String(f.name);
        if (fileSha256(path) != QLatin1String(f.sha256))
            return failAt(
                ctx, stage,
                QStringLiteral("%1 does not match its pinned hash").arg(QLatin1String(f.name)));
        if (path.endsWith(QLatin1String(".inf"), Qt::CaseInsensitive)) infPath = path;
        if (path.endsWith(QLatin1String(".cat"), Qt::CaseInsensitive)) catPath = path;
    }

    QString error;
    PublisherTrust trust;
    if (!trustCatalogSigner(catPath, trust, &error)) return failAt(ctx, stage, error);
    if (!createDeviceNode(&error)) return failAt(ctx, stage, error);
    bool reboot = false;
    if (!installPackage(infPath, &reboot, &error)) {
        // Leave no orphan node behind a failed install.
        QString ignored;
        removeDeviceNodes(&ignored);
        return failAt(ctx, stage, error);
    }
    ctx.result.ok = true;
    ctx.result.stage = stage;
    ctx.result.rebootRequired = reboot;
    Logger::info(QStringLiteral("[vdisplay-apply] driver installed%1")
                     .arg(reboot ? QStringLiteral(" (reboot required)") : QString()));
    return kExitOk;
}

// ── Stage "mode" ────────────────────────────────────────────────────────────

struct VirtualTarget
{
    LUID adapterId{};
    UINT32 targetId = 0;
    QString gdiName; // "\\.\DISPLAY3"
};

/// The virtual display among the active paths: an indirect-virtual output, or
/// the driver's monitor id in the device path.
std::optional<VirtualTarget> findVirtualTarget()
{
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS)
        return std::nullopt;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return std::nullopt;

    for (UINT32 i = 0; i < nPaths; ++i) {
        const DISPLAYCONFIG_PATH_INFO& p = paths[i];
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;

        const QString devPath = QString::fromWCharArray(tn.monitorDevicePath);
        const bool indirectVirtual = tn.outputTechnology == 17; // INDIRECT_VIRTUAL
        const bool mtt = devPath.contains(QLatin1String("MTT1337"), Qt::CaseInsensitive) ||
                         devPath.contains(QLatin1String("MttVDD"), Qt::CaseInsensitive);
        if (!indirectVirtual && !mtt) continue;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;

        VirtualTarget t;
        t.adapterId = p.targetInfo.adapterId;
        t.targetId = p.targetInfo.id;
        t.gdiName = QString::fromWCharArray(sn.viewGdiDeviceName);
        return t;
    }
    return std::nullopt;
}

int stageMode(Context& ctx)
{
    const QString stage = QStringLiteral("mode");
    if (ctx.request.action != VirtualDisplay::Request::Action::Add) {
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        return finish(ctx);
    }

    // The display appears a moment after the driver starts; ten seconds is
    // generous for a UMDF driver and a single monitor.
    std::optional<VirtualTarget> target;
    QElapsedTimer wait;
    wait.start();
    while (!(target = findVirtualTarget()) && wait.elapsed() < 10 * 1000)
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

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = DWORD(ctx.request.width);
    dm.dmPelsHeight = DWORD(ctx.request.height);
    dm.dmDisplayFrequency = DWORD(ctx.request.refresh);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    const std::wstring gdi = target->gdiName.toStdWString();
    const LONG rc =
        ChangeDisplaySettingsExW(gdi.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        // Not fatal: the display exists and streams at whatever mode it has.
        Logger::warning(QStringLiteral("[vdisplay-apply] mode %1x%2@%3 refused (%4) on %5")
                            .arg(ctx.request.width)
                            .arg(ctx.request.height)
                            .arg(ctx.request.refresh)
                            .arg(rc)
                            .arg(target->gdiName));
    } else {
        Logger::info(QStringLiteral("[vdisplay-apply] %1 set to %2x%3@%4")
                         .arg(target->gdiName)
                         .arg(ctx.request.width)
                         .arg(ctx.request.height)
                         .arg(ctx.request.refresh));
    }

    // HDR: the same call scripts/Set-DisplayHdr.ps1 makes. The target ids may
    // have moved with the mode change, so look the display up again.
    if (auto again = findVirtualTarget()) target = again;
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE ac{};
    ac.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    ac.header.size = sizeof(ac);
    ac.header.adapterId = target->adapterId;
    ac.header.id = target->targetId;
    ac.enableAdvancedColor = ctx.request.hdr ? 1 : 0;
    const LONG hr = DisplayConfigSetDeviceInfo(&ac.header);
    if (hr != ERROR_SUCCESS)
        Logger::warning(QStringLiteral("[vdisplay-apply] HDR %1 refused: %2")
                            .arg(ctx.request.hdr ? QStringLiteral("on") : QStringLiteral("off"))
                            .arg(lastErrorText(DWORD(hr))));

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
    if (stage == QLatin1String("driver") || stage == QLatin1String("all")) {
        const int rc = stageDriver(ctx);
        // stageDriver finishes itself on failure and on "remove"; on a
        // successful "add" it returns without writing so "all" can go on.
        if (!ctx.result.ok || ctx.result.stage == QLatin1String("done")) return rc;
        if (stage == QLatin1String("driver")) return finish(ctx);
    }
    if (stage == QLatin1String("mode") || stage == QLatin1String("all")) return stageMode(ctx);
    return failAt(ctx, QStringLiteral("request"), QStringLiteral("unknown stage %1").arg(stage));
#else
    Q_UNUSED(stage);
    return failAt(ctx, QStringLiteral("request"),
                  QStringLiteral("virtual displays are not supported on this platform"));
#endif
}

} // namespace VirtualDisplayApply
