/*
 * MoonlightWeb — native capture & encoding engine.
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

#include "StreamPriority.h"

#include "../../core/Log.h"

#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

// Newer than some SDKs this builds against (22000+ for the timer flag).
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace mw::native {

namespace {

std::string hex(long value)
{
    char out[16];
    std::snprintf(out, sizeof(out), "0x%08lX", static_cast<unsigned long>(value));
    return out;
}

bool envIs(const char* name, const char* value)
{
    char buf[32] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && _stricmp(buf, value) == 0;
}

void leaveEcoQos()
{
    if (envIs("MW_ECOQOS", "keep")) {
        log::info("[native] priority: MW_ECOQOS=keep — Windows may still throttle this process");
        return;
    }
    PROCESS_POWER_THROTTLING_STATE state = {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // In the mask with a zero state: "never throttle this", as opposed to
    // leaving the decision to the OS's heuristics.
    state.ControlMask =
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    if (::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &state,
                                sizeof(state)))
        log::info("[native] priority: out of EcoQoS (full-speed cores, timer requests honoured)");
    else
        log::info("[native] priority: EcoQoS opt-out refused (error " +
                  std::to_string(::GetLastError()) + ")");
}

// d3dkmthk.h is a WDK header; the one entry point needed is exported by
// gdi32, so it is looked up rather than linked.
enum KmtSchedulingClass : int
{
    KmtIdle,
    KmtBelowNormal,
    KmtNormal,
    KmtAboveNormal,
    KmtHigh
};
using SetSchedulingClassFn = LONG(APIENTRY*)(HANDLE, int);

void raiseGpuScheduling()
{
    HMODULE gdi = ::GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = ::LoadLibraryW(L"gdi32.dll");
    const auto set = gdi ? reinterpret_cast<SetSchedulingClassFn>(
                               ::GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"))
                         : nullptr;
    if (!set) {
        log::info("[native] priority: no GPU scheduling class on this Windows");
        return;
    }
    const LONG high = set(::GetCurrentProcess(), KmtHigh);
    if (high == 0) {
        log::info("[native] priority: GPU scheduling class HIGH");
        return;
    }
    const LONG above = set(::GetCurrentProcess(), KmtAboveNormal);
    log::info("[native] priority: GPU scheduling class HIGH refused (" + hex(high) + ")" +
              (above == 0 ? ", ABOVE_NORMAL instead" : ", left NORMAL"));
}

std::atomic<bool> g_ProcessDone{false};

} // namespace

void StreamPriority::engage()
{
    if (!g_ProcessDone.exchange(true)) {
        leaveEcoQos();
        if (!envIs("MW_GPU_PRIORITY", "normal"))
            raiseGpuScheduling();
        else
            log::info("[native] priority: MW_GPU_PRIORITY=normal — GPU scheduling left as is");
    }

    if (m_PowerRequest) return;
    REASON_CONTEXT reason = {};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString =
        const_cast<LPWSTR>(L"MoonlightWeb is streaming this display");
    m_PowerRequest = ::PowerCreateRequest(&reason);
    if (m_PowerRequest == INVALID_HANDLE_VALUE) m_PowerRequest = nullptr;
    if (!m_PowerRequest || !::PowerSetRequest(m_PowerRequest, PowerRequestSystemRequired) ||
        !::PowerSetRequest(m_PowerRequest, PowerRequestDisplayRequired)) {
        log::info("[native] priority: could not keep the machine awake (error " +
                  std::to_string(::GetLastError()) + ")");
    }
}

void StreamPriority::release()
{
    if (!m_PowerRequest) return;
    ::PowerClearRequest(m_PowerRequest, PowerRequestDisplayRequired);
    ::PowerClearRequest(m_PowerRequest, PowerRequestSystemRequired);
    ::CloseHandle(m_PowerRequest);
    m_PowerRequest = nullptr;
}

void StreamPriority::raiseDevice(ID3D11Device* device, const char* role)
{
    if (!device || envIs("MW_GPU_PRIORITY", "normal")) return;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) return;
    const HRESULT hr = dxgi->SetGPUThreadPriority(7);
    if (SUCCEEDED(hr))
        log::info(std::string("[native] priority: GPU thread priority 7 on the ") + role +
                  " device");
    else
        log::info(std::string("[native] priority: GPU thread priority refused on the ") + role +
                  " device (" + hex(hr) + ")");
}

} // namespace mw::native
