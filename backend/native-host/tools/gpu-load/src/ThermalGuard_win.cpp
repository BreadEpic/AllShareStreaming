/*
 * MoonlightWeb — native capture & encoding engine: GPU load tool.
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

#include "ThermalGuard.h"

#include <windows.h>
#include <winternl.h>

#include <d3dkmthk.h>

// D3DKMT's KMTQAITYPE_ADAPTERPERFDATA is the counter Task Manager reads for a
// GPU's temperature. It is the kernel's, not a vendor SDK's: the same call
// answered for the Arc A380, the RTX 5060 Ti and the AMD iGPU of DualRTX
// (22/09). A driver that does not report leaves the field at 0, which is read
// as "no sensor", never as a cold GPU.
struct ThermalGuard::Impl
{
    D3DKMT_HANDLE adapter = 0;
    double limit = 0.0;
    bool sensor = false;

    bool query(KMTQUERYADAPTERINFOTYPE type, void* data, UINT size) const
    {
        D3DKMT_QUERYADAPTERINFO q = {};
        q.hAdapter = adapter;
        q.Type = type;
        q.pPrivateDriverData = data;
        q.PrivateDriverDataSize = size;
        return D3DKMTQueryAdapterInfo(&q) == 0;
    }
};

ThermalGuard::ThermalGuard(const GpuEntry& gpu)
    : d(std::make_unique<Impl>())
{
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid.LowPart = gpu.luidLow;
    open.AdapterLuid.HighPart = gpu.luidHigh;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0) return;
    d->adapter = open.hAdapter;
    D3DKMT_ADAPTER_PERFDATACAPS caps = {};
    if (d->query(KMTQAITYPE_ADAPTERPERFDATA_CAPS, &caps, sizeof(caps)) && caps.TemperatureMax)
        d->limit = caps.TemperatureMax / 10.0;
    d->sensor = read().ok;
}

ThermalGuard::~ThermalGuard()
{
    if (d->adapter) {
        D3DKMT_CLOSEADAPTER close = {d->adapter};
        D3DKMTCloseAdapter(&close);
    }
}

bool ThermalGuard::hasSensor() const
{
    return d->sensor;
}

QString ThermalGuard::source() const
{
    return d->sensor ? QStringLiteral("D3DKMT adapter perf data") : QStringLiteral("none");
}

double ThermalGuard::deviceLimitCelsius() const
{
    return d->limit;
}

ThermalReading ThermalGuard::read()
{
    ThermalReading r;
    if (!d->adapter) return r;
    D3DKMT_ADAPTER_PERFDATA perf = {};
    if (!d->query(KMTQAITYPE_ADAPTERPERFDATA, &perf, sizeof(perf)) || perf.Temperature == 0)
        return r;
    r.ok = true;
    r.celsius = perf.Temperature / 10.0;
    return r;
}
