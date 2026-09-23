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

#include <d3d11.h>
#include <windows.h>

namespace mw::native {

/// What a streaming host asks of Windows so that power management does not
/// sit between the desktop and the viewer. Everything here is best effort,
/// works without elevation, and is logged once whether it took or not — a
/// refusal leaves the session exactly as it was before this existed.
///
/// ── Why each one ─────────────────────────────────────────────────────────────
///
/// EcoQoS. This process has no window. Windows 11 reads that as background
/// work and, on a hybrid CPU or under "Best power efficiency", runs it on the
/// efficiency cores at a lowered clock and ignores its timer requests. The
/// capture thread's MMCSS standing does not lift that: it is a process-wide
/// classification. Opting out is one call. MW_ECOQOS=keep leaves it in place,
/// for a bench that wants the before and after.
///
/// GPU scheduling. The converter's shaders share the GPU's queue with the
/// game being streamed; a game that keeps it full makes every capture wait
/// its turn, and that is a frame late or a frame dropped. The process is put
/// in the HIGH scheduling class and each of its devices at the top thread
/// priority, which is what Sunshine does too. REALTIME is never asked for: it
/// needs SeIncreaseBasePriorityPrivilege, which a non-elevated token does not
/// carry, and under hardware-accelerated scheduling it can starve the desktop
/// itself. The encoder is fixed-function and has its own engine; this is
/// about the shaders in front of it. MW_GPU_PRIORITY=normal skips it;
/// MW_GPU_PRIORITY=realtime (a bench switch) enables the privilege where the
/// token holds it — the SYSTEM or elevated worker — and asks REALTIME, HIGH
/// when refused. Whether the GPU schedules itself (HAGS) is logged per device,
/// since it decides what a class means. MW_CPU_PRIORITY=high (a bench switch
/// too) raises the process's CPU class.
///
/// Staying awake. A viewer watching without touching anything sends no input,
/// and the machine would sleep or blank its screen under the stream — which
/// the viewer sees as the picture going black. Held for the session only.
class StreamPriority
{
public:
    StreamPriority() = default;
    ~StreamPriority() { release(); }
    StreamPriority(const StreamPriority&) = delete;
    StreamPriority& operator=(const StreamPriority&) = delete;

    /// For the session's life: the process-wide opt-outs (applied once per
    /// process, they are harmless when idle) and the power request.
    void engage();

    /// Drops the power request. Idempotent.
    void release();

    /// Each device the pipeline draws on, as it is (re)built.
    static void raiseDevice(ID3D11Device* device, const char* role);

private:
    HANDLE m_PowerRequest = nullptr;
};

} // namespace mw::native
