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

#include "D3d12Context.h"

#include "../../../core/Log.h"

#include <d3d12sdklayers.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace mw::native::platform {

namespace {

// Desktop Duplication rotates through a few surfaces; past this many, the
// capture has been rebuilt under us and the old entries are dead weight.
constexpr size_t kMaxSurfaces = 8;

// A GPU that takes a second over a few dispatches is not slow, it is gone —
// and a session that waits for ever on it is a stream that freezes with no
// line in the log.
constexpr DWORD kGpuTimeoutMs = 1000;

} // namespace

std::string D3d12Context::hresultToString(HRESULT hr)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

D3d12Context::~D3d12Context()
{
    close();
}

void D3d12Context::close()
{
    m_Surfaces.clear();
    m_SharedFence11.Reset();
    m_SharedFence.Reset();
    m_SharedFenceFor = nullptr;
    m_SharedFenceRefused = false;
    m_SharedValue = m_SharedWaited = 0;
    m_List.Reset();
    m_Allocator.Reset();
    m_Fence.Reset();
    m_ComputeQueue.Reset();
    m_Device.Reset();
    if (m_Event) {
        ::CloseHandle(m_Event);
        m_Event = nullptr;
    }
    m_FenceValue = 0;
}

bool D3d12Context::init(ID3D11Device* captureDevice, std::string& error)
{
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (!captureDevice || FAILED(captureDevice->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetAdapter(&adapter))) {
        error = "the capture's adapter is unknown";
        return false;
    }
    return init(adapter.Get(), error);
}

bool D3d12Context::init(IDXGIAdapter* adapter, std::string& error)
{
    close();

    // MW_D3D12_DEBUG=1: the validation layer (it ships with the "Graphics
    // Tools" optional feature), its messages relayed to the log after every
    // submission. A driver that dislikes a barrier says nothing otherwise — it
    // returns E_INVALIDARG from Close(), or simply crashes.
    char value[8] = {};
    const DWORD n = ::GetEnvironmentVariableA("MW_D3D12_DEBUG", value, sizeof(value));
    m_Debug = n > 0 && n < sizeof(value) && value[0] == '1';
    if (m_Debug) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    }

    HRESULT hr = ::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device));
    if (FAILED(hr)) {
        error = "no D3D12 on this GPU (" + hresultToString(hr) + ")";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue = {};
    queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    // HIGH, not GLOBAL_REALTIME: the latter needs a privilege this process
    // does not hold (the host runs unelevated), and refusing the queue over it
    // would cost the whole path.
    queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    hr = m_Device->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_ComputeQueue));
    if (SUCCEEDED(hr))
        hr = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                              IID_PPV_ARGS(&m_Allocator));
    if (SUCCEEDED(hr))
        hr = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_Allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&m_List));
    if (SUCCEEDED(hr)) hr = m_List->Close();
    if (SUCCEEDED(hr)) hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
    if (SUCCEEDED(hr)) {
        m_Event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_Event) hr = HRESULT_FROM_WIN32(::GetLastError());
    }
    if (FAILED(hr)) {
        error = "no D3D12 compute queue on this GPU (" + hresultToString(hr) + ")";
        close();
        return false;
    }

    const DWORD t = ::GetEnvironmentVariableA("MW_D3D12_TIMING", value, sizeof(value));
    m_Timing = t > 0 && t < sizeof(value) && value[0] == '1';
    if (m_Timing) {
        D3D12_QUERY_HEAP_DESC heap = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
        D3D12_HEAP_PROPERTIES readback = {};
        readback.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 2 * sizeof(UINT64);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        m_Timing = SUCCEEDED(m_Device->CreateQueryHeap(&heap, IID_PPV_ARGS(&m_TimingHeap))) &&
                   SUCCEEDED(m_Device->CreateCommittedResource(
                       &readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                       nullptr, IID_PPV_ARGS(&m_TimingReadback))) &&
                   SUCCEEDED(m_ComputeQueue->GetTimestampFrequency(&m_TimingFrequency)) &&
                   m_TimingFrequency > 0;
    }
    return true;
}

ID3D12GraphicsCommandList* D3d12Context::begin(std::string& error)
{
    if (!m_List) {
        error = "the D3D12 context is not initialized";
        return nullptr;
    }
    HRESULT hr = m_Allocator->Reset();
    if (SUCCEEDED(hr)) hr = m_List->Reset(m_Allocator.Get(), nullptr);
    if (FAILED(hr)) {
        error = "could not reset the compute command list (" + hresultToString(hr) + ")";
        return nullptr;
    }
    if (m_Timing) m_List->EndQuery(m_TimingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    return m_List.Get();
}

void D3d12Context::relayMessages()
{
    ComPtr<ID3D12InfoQueue> queue;
    if (!m_Debug || !m_Device || FAILED(m_Device.As(&queue))) return;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        queue->GetMessage(i, nullptr, &length);
        std::vector<char> bytes(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (SUCCEEDED(queue->GetMessage(i, message, &length)))
            log::warning(std::string("[native] D3D12: ") + message->pDescription);
    }
    queue->ClearStoredMessages();
}

bool D3d12Context::submitAndWait(std::string& error)
{
    if (m_Timing) {
        m_List->EndQuery(m_TimingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        m_List->ResolveQueryData(m_TimingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                 m_TimingReadback.Get(), 0);
    }
    LARGE_INTEGER started = {}, frequency = {};
    ::QueryPerformanceCounter(&started);
    HRESULT hr = m_List->Close();
    relayMessages();
    if (FAILED(hr)) {
        error = "the compute command list was refused (" + hresultToString(hr) + ")";
        return false;
    }
    if (m_SharedValue > m_SharedWaited) {
        m_ComputeQueue->Wait(m_SharedFence.Get(), m_SharedValue);
        m_SharedWaited = m_SharedValue;
    }
    ID3D12CommandList* lists[] = {m_List.Get()};
    m_ComputeQueue->ExecuteCommandLists(1, lists);
    hr = m_ComputeQueue->Signal(m_Fence.Get(), ++m_FenceValue);
    if (SUCCEEDED(hr)) hr = m_Fence->SetEventOnCompletion(m_FenceValue, m_Event);
    if (FAILED(hr)) {
        error = "could not wait for the compute queue (" + hresultToString(hr) + ")";
        return false;
    }
    if (::WaitForSingleObject(m_Event, kGpuTimeoutMs) != WAIT_OBJECT_0) {
        error = "the compute queue did not answer (device: " +
                hresultToString(m_Device->GetDeviceRemovedReason()) + ")";
        return false;
    }

    UINT64* stamps = nullptr;
    const D3D12_RANGE read = {0, 2 * sizeof(UINT64)};
    if (m_Timing && SUCCEEDED(m_TimingReadback->Map(0, &read, reinterpret_cast<void**>(&stamps)))) {
        LARGE_INTEGER ended = {};
        ::QueryPerformanceCounter(&ended);
        ::QueryPerformanceFrequency(&frequency);
        const double gpuMs = stamps[1] >= stamps[0]
                                 ? 1000.0 * static_cast<double>(stamps[1] - stamps[0]) /
                                       static_cast<double>(m_TimingFrequency)
                                 : 0.0;
        const double wallMs = 1000.0 * static_cast<double>(ended.QuadPart - started.QuadPart) /
                              static_cast<double>(frequency.QuadPart);
        const D3D12_RANGE nothing = {0, 0};
        m_TimingReadback->Unmap(0, &nothing);
        m_TimingGpuMs += gpuMs;
        m_TimingWallMs += wallMs;
        if (gpuMs > m_TimingGpuMaxMs) m_TimingGpuMaxMs = gpuMs;
        if (wallMs > m_TimingWallMaxMs) m_TimingWallMaxMs = wallMs;
        if (++m_TimingFrames % 300 == 0) {
            char line[200];
            std::snprintf(line, sizeof(line),
                          "[native] D3D12 compute, last 300 submissions: GPU work %.2f ms mean "
                          "(max %.2f), submit to done %.2f ms mean (max %.2f)",
                          m_TimingGpuMs / 300.0, m_TimingGpuMaxMs, m_TimingWallMs / 300.0,
                          m_TimingWallMaxMs);
            log::info(line);
            m_TimingGpuMs = m_TimingWallMs = m_TimingGpuMaxMs = m_TimingWallMaxMs = 0.0;
        }
    }
    return true;
}

void D3d12Context::after(ID3D11DeviceContext* context)
{
    if (!context || !m_Device) return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device.Get() != m_SharedFenceFor) {
        m_SharedFence11.Reset();
        m_SharedFence.Reset();
        m_SharedFenceFor = device.Get();
        m_SharedFenceRefused = false;
        m_SharedValue = m_SharedWaited = 0;
    }
    if (!m_SharedFence11 && !m_SharedFenceRefused) {
        ComPtr<ID3D11Device5> device5;
        HANDLE handle = nullptr;
        HRESULT hr =
            m_Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_SharedFence));
        if (SUCCEEDED(hr))
            hr = m_Device->CreateSharedHandle(m_SharedFence.Get(), nullptr, GENERIC_ALL, nullptr,
                                              &handle);
        if (SUCCEEDED(hr)) hr = device.As(&device5);
        if (SUCCEEDED(hr)) hr = device5->OpenSharedFence(handle, IID_PPV_ARGS(&m_SharedFence11));
        if (handle) ::CloseHandle(handle);
        if (FAILED(hr)) {
            m_SharedFenceRefused = true;
            m_SharedFence11.Reset();
            m_SharedFence.Reset();
            log::info("[native] D3D12: no fence shared with the D3D11 device (" +
                      hresultToString(hr) + ") — its writes are flushed, not waited for");
        }
    }
    ComPtr<ID3D11DeviceContext4> context4;
    if (m_SharedFence11 && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4))) &&
        SUCCEEDED(context4->Signal(m_SharedFence11.Get(), m_SharedValue + 1)))
        ++m_SharedValue;
    // The signal only counts once it has left the D3D11 runtime for the queue.
    context->Flush();
}

ID3D12Resource* D3d12Context::open(ID3D11Texture2D* captured, std::string& error)
{
    for (const Surface& surface : m_Surfaces)
        if (surface.d3d11.Get() == captured) return surface.d3d12.Get();

    ComPtr<IDXGIResource1> shareable;
    HANDLE handle = nullptr;
    HRESULT hr = captured->QueryInterface(IID_PPV_ARGS(&shareable));
    if (SUCCEEDED(hr))
        hr = shareable->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &handle);
    ComPtr<ID3D12Resource> opened;
    if (SUCCEEDED(hr)) {
        hr = m_Device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
        ::CloseHandle(handle);
    }
    if (FAILED(hr)) {
        error = "the captured surface cannot be opened in D3D12 (" + hresultToString(hr) + ")";
        return nullptr;
    }

    if (m_Surfaces.size() >= kMaxSurfaces) m_Surfaces.clear();
    Surface surface;
    surface.d3d11 = captured;
    surface.d3d12 = opened;
    m_Surfaces.push_back(std::move(surface));
    return m_Surfaces.back().d3d12.Get();
}

} // namespace mw::native::platform
