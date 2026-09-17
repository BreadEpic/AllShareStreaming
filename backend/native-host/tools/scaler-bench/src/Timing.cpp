/*
 * MoonlightWeb — native capture & encoding engine: scaler bench.
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

#include "Timing.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace bench {

bool GpuTimer::init(ID3D11Device* device, ID3D11DeviceContext* context, int batchSize,
                    std::string& error)
{
    m_Context = context;
    m_BatchSize = batchSize;
    D3D11_QUERY_DESC disjoint = {};
    disjoint.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    const HRESULT hr = device->CreateQuery(&disjoint, m_Disjoint.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        std::ostringstream why;
        why << "could not create the disjoint query (0x" << std::hex << hr
            << ", device removed reason 0x" << device->GetDeviceRemovedReason() << ")";
        error = why.str();
        return false;
    }
    D3D11_QUERY_DESC event = {};
    event.Query = D3D11_QUERY_EVENT;
    if (FAILED(device->CreateQuery(&event, m_Event.ReleaseAndGetAddressOf()))) {
        error = "could not create the event query";
        return false;
    }
    D3D11_QUERY_DESC stamp = {};
    stamp.Query = D3D11_QUERY_TIMESTAMP;
    m_Begin.resize(batchSize);
    m_End.resize(batchSize);
    for (int i = 0; i < batchSize; ++i) {
        if (FAILED(device->CreateQuery(&stamp, m_Begin[i].ReleaseAndGetAddressOf())) ||
            FAILED(device->CreateQuery(&stamp, m_End[i].ReleaseAndGetAddressOf()))) {
            error = "could not create the timestamp queries";
            return false;
        }
    }
    return true;
}

bool GpuTimer::measureBatch(const std::function<void()>& pass, std::vector<double>& outUs)
{
    m_Context->Begin(m_Disjoint.Get());
    for (int i = 0; i < m_BatchSize; ++i) {
        m_Context->End(m_Begin[i].Get());
        pass();
        m_Context->End(m_End[i].Get());
    }
    m_Context->End(m_Disjoint.Get());
    m_Context->Flush();

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    while (m_Context->GetData(m_Disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE)
        ::Sleep(0);
    if (dj.Disjoint || dj.Frequency == 0) return false;

    for (int i = 0; i < m_BatchSize; ++i) {
        UINT64 t0 = 0;
        UINT64 t1 = 0;
        while (m_Context->GetData(m_Begin[i].Get(), &t0, sizeof(t0), 0) == S_FALSE)
            ::Sleep(0);
        while (m_Context->GetData(m_End[i].Get(), &t1, sizeof(t1), 0) == S_FALSE)
            ::Sleep(0);
        outUs.push_back(static_cast<double>(t1 - t0) * 1e6 / static_cast<double>(dj.Frequency));
    }
    return true;
}

void GpuTimer::drain()
{
    m_Context->End(m_Event.Get());
    m_Context->Flush();
    BOOL done = FALSE;
    while (m_Context->GetData(m_Event.Get(), &done, sizeof(done), 0) == S_FALSE)
        ::Sleep(0);
}

void GpuTimer::measureBatchWall(const std::function<void()>& pass, std::vector<double>& outUs)
{
    LARGE_INTEGER freq = {};
    ::QueryPerformanceFrequency(&freq);
    for (int i = 0; i < m_BatchSize; ++i) {
        // Drain what came before, so the clock starts on an idle queue.
        m_Context->End(m_Event.Get());
        m_Context->Flush();
        BOOL done = FALSE;
        while (m_Context->GetData(m_Event.Get(), &done, sizeof(done), 0) == S_FALSE)
            ::Sleep(0);

        LARGE_INTEGER t0 = {};
        LARGE_INTEGER t1 = {};
        ::QueryPerformanceCounter(&t0);
        pass();
        m_Context->End(m_Event.Get());
        m_Context->Flush();
        while (m_Context->GetData(m_Event.Get(), &done, sizeof(done), 0) == S_FALSE)
            ::Sleep(0);
        ::QueryPerformanceCounter(&t1);
        outUs.push_back(static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 /
                        static_cast<double>(freq.QuadPart));
    }
}

TimeStats computeStats(std::vector<double> us, double trimEachSide, int discarded)
{
    TimeStats s;
    s.discarded = discarded;
    s.n = static_cast<int>(us.size());
    if (us.empty()) return s;
    std::sort(us.begin(), us.end());
    s.minUs = us.front();
    s.medianUs = us[us.size() / 2];
    s.p95Us = us[std::min(us.size() - 1, static_cast<size_t>(std::floor(us.size() * 0.95)))];
    const size_t cut = static_cast<size_t>(std::floor(us.size() * trimEachSide));
    const size_t first = cut;
    const size_t last = us.size() - cut;
    if (last > first) {
        s.trimmedUs = std::accumulate(us.begin() + first, us.begin() + last, 0.0) /
                      static_cast<double>(last - first);
    } else {
        s.trimmedUs = s.medianUs;
    }
    return s;
}

} // namespace bench
