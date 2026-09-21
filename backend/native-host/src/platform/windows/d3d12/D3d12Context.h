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

#pragma once

#ifndef NOMINMAX
#define NOMINMAX // <windows.h>'s min/max macros break std::min/std::max
#endif

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace mw::native::platform {

/// The D3D12 device a session's picture path runs on, and the one queue of it
/// that matters: COMPUTE.
///
/// ── Why D3D12 at all ────────────────────────────────────────────────────────
///
/// A D3D11 device has one queue, and it is the 3D one — the queue a game fills.
/// Everything the D3D11 pipeline asks of the GPU waits its turn there: measured
/// on the Arc A380 under Resident Evil Requiem (21/09/2026), the conversion of
/// one frame took 13.4 ms on average and 74 ms at p99, of which the work itself
/// is well under a millisecond. The rest is the queue.
///
/// D3D12 lets a process open other queues on the same GPU. A COMPUTE queue is
/// scheduled beside the 3D one rather than behind it ("async compute"): the
/// same dispatches, probed on the same machine under the same game, came back
/// in ~2 ms at the median. That is the whole reason for this directory.
///
/// ── What stays D3D11 ────────────────────────────────────────────────────────
///
/// Capture. Desktop Duplication only duplicates onto a D3D11 device, and there
/// is no D3D12 equivalent. But the surface it hands over is shareable: opened
/// here through its NT handle, it is the same VRAM, read without a copy — what
/// CrossGpuBridge already does for its readback, and measured to work on the
/// Arc and on the RTX 5060 Ti alike. Windows.Graphics.Capture surfaces have no
/// such handle, which is one of the reasons a session can be refused this path
/// and keep the D3D11 one.
class D3d12Context
{
public:
    D3d12Context() = default;
    ~D3d12Context();

    D3d12Context(const D3d12Context&) = delete;
    D3d12Context& operator=(const D3d12Context&) = delete;

    /// A device on the adapter @p captureDevice lives on. False, with the reason
    /// in @p error, on a machine or a driver that has no D3D12 — the caller
    /// keeps its D3D11 pipeline and says why.
    bool init(ID3D11Device* captureDevice, std::string& error);

    /// The same on a named adapter. What the tests use, on WARP.
    bool init(IDXGIAdapter* adapter, std::string& error);

    ID3D12Device* device() const { return m_Device.Get(); }
    ID3D12CommandQueue* computeQueue() const { return m_ComputeQueue.Get(); }

    /// The compute command list, reset and ready to record. One frame at a
    /// time: the session's loop is synchronous by design, and so is this.
    ID3D12GraphicsCommandList* begin(std::string& error);

    /// Close what begin() returned, run it, and wait for the GPU to be done
    /// with it. The wait is the point, not a shortcut: Desktop Duplication's
    /// surface may only be released once nothing reads it any more, and the
    /// session releases it right after.
    bool submitAndWait(std::string& error);

    /// @p captured, as this device sees it. Opened once per texture and kept:
    /// Desktop Duplication rotates through a handful of surfaces, and opening
    /// one costs a kernel round trip. Null, with the reason, for a surface that
    /// cannot be shared (Windows.Graphics.Capture's).
    ID3D12Resource* open(ID3D11Texture2D* captured, std::string& error);

    /// Forget the opened surfaces — the capture was restarted, and the old ones
    /// would only pin VRAM nobody draws to any more.
    void forgetSurfaces() { m_Surfaces.clear(); }

    static std::string hresultToString(HRESULT hr);

private:
    void close();
    void relayMessages();

    struct Surface
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11;
        Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
    };

    Microsoft::WRL::ComPtr<ID3D12Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_ComputeQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_Allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_List;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
    UINT64 m_FenceValue = 0;
    HANDLE m_Event = nullptr;
    bool m_Debug = false;
    std::vector<Surface> m_Surfaces;
};

} // namespace mw::native::platform
