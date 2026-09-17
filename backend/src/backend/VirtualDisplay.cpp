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

#include "backend/VirtualDisplay.h"

#include "common/Edition.h"
#include "common/Logger.h"
#include "common/WinSystemPath.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>

#ifdef Q_OS_WIN
#include "backend/streambackend/NativeProbeService.h"
#include "mw/native/Capabilities.h"

#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <comdef.h>
#include <taskschd.h>
#include <vector>
#endif

namespace VirtualDisplay {

// ── Paths ───────────────────────────────────────────────────────────────────

QString stagingDir()
{
#ifdef Q_OS_WIN
    // Per-user and under the edition's own name, like SelfUpdater and
    // GamepadDriver: an elevated task registered for this user runs a fixed
    // command, and a DEV install must never stage into production's.
    const QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (!base.isEmpty() && !mw::win::isUnderSystem32(base))
        return base + QLatin1Char('/') + mw::edition::productName() + QStringLiteral("/vdisplay");
    // A service has SYSTEM's profile, under System32. The install directory is
    // admin-only, which is the property that matters.
    return QCoreApplication::applicationDirPath() + QStringLiteral("/vdisplay");
#else
    return QDir::tempPath() + QStringLiteral("/MoonlightWeb-vdisplay");
#endif
}

QString requestPath()
{
    return stagingDir() + QStringLiteral("/request.json");
}

QString resultPath()
{
    return stagingDir() + QStringLiteral("/result.json");
}

QString driverDir()
{
    return stagingDir() + QStringLiteral("/driver");
}

QString archivePath()
{
    return stagingDir() + QStringLiteral("/driver.zip");
}

// ── Status ──────────────────────────────────────────────────────────────────

QJsonObject toJson(const Status& st, bool admin)
{
    QJsonObject obj;
    obj["supported"] = st.supported;
    obj["installed"] = st.installed;
    obj["active"] = st.active;
    if (!admin) return obj;

    obj["can_install"] = st.canInstall;
    obj["method"] = st.method;
    obj["download_url"] = downloadUrl();
    obj["os_hdr_capable"] = st.osHdrCapable;

    QJsonArray displays;
    for (const ActiveDisplay& d : st.activeDisplays) {
        QJsonObject o;
        o["id"] = d.id;
        o["label"] = d.label;
        o["width"] = d.width;
        o["height"] = d.height;
        o["refresh_mhz"] = d.refreshMilliHz;
        o["hdr_active"] = d.hdrActive;
        displays.append(o);
    }
    obj["active_displays"] = displays;

    QJsonArray gpus;
    for (const Gpu& g : st.gpus) {
        QJsonObject o;
        o["id"] = g.id;
        o["name"] = g.name;
        gpus.append(o);
    }
    obj["gpus"] = gpus;

    QJsonObject presets;
    QJsonArray res;
    for (const Preset& p : resolutionPresets())
        res.append(QJsonArray{p.width, p.height});
    QJsonArray hz;
    for (int r : refreshPresets())
        hz.append(r);
    presets["resolutions"] = res;
    presets["refresh"] = hz;
    obj["presets"] = presets;
    return obj;
}

#ifdef Q_OS_WIN

namespace {

struct ComScope
{
    HRESULT hr;
    ComScope()
        : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {}
    ~ComScope()
    {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

template <typename T> struct ComPtr
{
    T* p = nullptr;
    ~ComPtr()
    {
        if (p) p->Release();
    }
    T** out() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct Bstr
{
    BSTR b;
    explicit Bstr(const QString& s)
        : b(SysAllocString(reinterpret_cast<const OLECHAR*>(s.utf16())))
    {}
    ~Bstr() { SysFreeString(b); }
};

QString bstrToQString(BSTR b)
{
    return b ? QString::fromWCharArray(b, static_cast<int>(SysStringLen(b))) : QString();
}

/// Strip surrounding quotes and normalise separators for a path comparison.
QString canonicalExe(QString s)
{
    s = s.trimmed();
    if (s.size() >= 2 && s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
        s = s.mid(1, s.size() - 2);
    return QDir::cleanPath(QDir::fromNativeSeparators(s)).toLower();
}

/// The installer's task exists AND its action is this very exe with the fixed
/// argument. Both checked: a task pointing elsewhere (an install moved, an
/// older layout) must not be reported as a way to elevate.
bool elevatedTaskUsable()
{
    ComScope com;
    if (!com.usable()) return false;
    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(service.out()));
    if (FAILED(hr)) return false;
    VARIANT none;
    VariantInit(&none);
    if (FAILED(service->Connect(none, none, none, none))) return false;
    ComPtr<ITaskFolder> root;
    Bstr rootPath(QStringLiteral("\\"));
    if (FAILED(service->GetFolder(rootPath.b, root.out()))) return false;

    Bstr name(taskName());
    ComPtr<IRegisteredTask> task;
    if (FAILED(root->GetTask(name.b, task.out())) || !task) return false;

    ComPtr<ITaskDefinition> def;
    if (FAILED(task->get_Definition(def.out())) || !def) return false;
    ComPtr<IActionCollection> actions;
    if (FAILED(def->get_Actions(actions.out())) || !actions) return false;
    ComPtr<IAction> action;
    if (FAILED(actions->get_Item(1, action.out())) || !action) return false;
    ComPtr<IExecAction> exec;
    if (FAILED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.out()))) ||
        !exec)
        return false;

    BSTR path = nullptr;
    BSTR args = nullptr;
    exec->get_Path(&path);
    exec->get_Arguments(&args);
    const QString taskExe = canonicalExe(bstrToQString(path));
    const QString taskArgs = bstrToQString(args).trimmed();
    SysFreeString(path);
    SysFreeString(args);

    const QString ourExe = canonicalExe(QCoreApplication::applicationFilePath());
    if (taskExe != ourExe) {
        Logger::info(QStringLiteral("[vdisplay] task \"%1\" runs %2, not this exe — unusable")
                         .arg(taskName(), taskExe));
        return false;
    }
    return taskArgs == applyArgument();
}

/// Cached: the answer changes only with an install, and each call is a COM
/// round trip the hosts page would otherwise pay on every poll.
QString installMethod()
{
    static QString cached;
    static QElapsedTimer age;
    if (age.isValid() && age.elapsed() < 30 * 1000) return cached;

    if (processElevated())
        cached = QStringLiteral("elevated");
    else if (elevatedTaskUsable())
        cached = QStringLiteral("task");
    else
        cached.clear();
    age.restart();
    return cached;
}

bool osHdrCapable()
{
    // HDR on a virtual display needs IddCx 1.10 (Windows 11 23H2, build 22631).
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return false;
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (fn(&v) != 0) return false;
    return v.dwBuildNumber >= 22631;
}

} // namespace

bool driverPresent()
{
    // The device node, asked of the configuration manager: a driver package
    // left in the store without its node (the state after "remove") does not
    // count, and neither does a registry key that outlived an uninstall.
    const std::wstring filter = hardwareId().toStdWString();
    ULONG len = 0;
    // Enumerate the root-enumerated devices, then match the hardware id
    // ourselves: the configuration manager filters by enumerator, not by id.
    if (CM_Get_Device_ID_List_SizeW(&len, L"ROOT", CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS ||
        len == 0)
        return false;
    std::vector<wchar_t> buf(len);
    if (CM_Get_Device_ID_ListW(L"ROOT", buf.data(), len, CM_GETIDLIST_FILTER_ENUMERATOR) !=
        CR_SUCCESS)
        return false;

    for (const wchar_t* id = buf.data(); *id; id += wcslen(id) + 1) {
        DEVINST inst = 0;
        if (CM_Locate_DevNodeW(&inst, const_cast<DEVINSTID_W>(id), CM_LOCATE_DEVNODE_NORMAL) !=
            CR_SUCCESS)
            continue;
        ULONG size = 0;
        if (CM_Get_DevNode_Registry_PropertyW(inst, CM_DRP_HARDWAREID, nullptr, nullptr, &size,
                                              0) != CR_BUFFER_SMALL ||
            size == 0)
            continue;
        std::vector<wchar_t> ids(size / sizeof(wchar_t) + 1, 0);
        if (CM_Get_DevNode_Registry_PropertyW(inst, CM_DRP_HARDWAREID, nullptr, ids.data(), &size,
                                              0) != CR_SUCCESS)
            continue;
        for (const wchar_t* h = ids.data(); *h; h += wcslen(h) + 1)
            if (_wcsicmp(h, filter.c_str()) == 0) return true;
    }
    return false;
}

bool processElevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok =
        ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

Status probe()
{
    Status st;
#if defined(Q_PROCESSOR_ARM)
    // The native engine does not run on Windows ARM64, and the ARM64 driver
    // package would need its own pinned hashes: nothing to offer there.
    return st;
#else
    st.supported = true;
    st.installed = driverPresent();
    st.method = installMethod();
    st.canInstall = !st.method.isEmpty();
    st.osHdrCapable = osHdrCapable();

    const mw::native::Capabilities caps = NativeProbeService::instance().snapshot();
    for (const mw::native::DisplayInfo& d : caps.displays) {
        if (d.kind != mw::native::DisplayKind::Virtual) continue;
        ActiveDisplay a;
        a.id = d.id;
        a.label = QString::fromStdString(d.label);
        a.width = d.width;
        a.height = d.height;
        a.refreshMilliHz = d.refreshMilliHz;
        a.hdrActive = d.hdrActive;
        st.activeDisplays.append(a);
    }
    st.active = !st.activeDisplays.isEmpty();
    for (const mw::native::GpuInfo& g : caps.gpus) {
        Gpu gpu;
        gpu.id = g.id;
        gpu.name = QString::fromStdString(g.name);
        // Every indirect display driver present (Parsec, this one) makes DXGI
        // list each real adapter once more under the same name. The driver
        // picks its GPU by name, so one entry per name is the honest list.
        bool seen = false;
        for (const Gpu& have : st.gpus)
            if (have.name == gpu.name) seen = true;
        if (!seen) st.gpus.append(gpu);
    }
    return st;
#endif
}

#else // not Windows: step 2 (macOS, CGVirtualDisplay) and 3 (Linux, portal VIRTUAL)

bool driverPresent()
{
    return false;
}

bool processElevated()
{
    return false;
}

Status probe()
{
    return Status{};
}

#endif

} // namespace VirtualDisplay
