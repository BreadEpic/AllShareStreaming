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

#include "mw/native/Capabilities.h"
#include "mw/native/SessionConfig.h"

#include <string>

namespace mw::native {

/// Everything the "Detect → Optimize" half of the engine decides, before a
/// single platform object is created.
struct Selection
{
    const DisplayInfo* display = nullptr;
    const GpuInfo* gpu = nullptr;

    Codec codec = Codec::H264;
    EncoderApi encoder = EncoderApi::None;

    int width = 0;
    int height = 0;
    int fps = 0;

    /// 10-bit, and therefore actually HDR. False when HDR was asked for but the
    /// display is in SDR or the encoder has no 10-bit path — the session then
    /// runs SDR rather than failing.
    bool hdr = false;

    /// The encoder would carry HDR on an HDR display — see
    /// SessionInfo::hdrCapable. Independent of the request and of the display's
    /// current mode, which are the two things that change under a session.
    bool hdrCapable = false;

    /// 4:4:4 granted: asked for AND the chosen codec has it on this encoder.
    /// The request steers the codec choice (see select()), so this is false
    /// only when no codec the client and the GPU share can carry 4:4:4 — the
    /// session then runs 4:2:0 rather than failing.
    bool yuv444 = false;

    /// The chosen GPU is not the one driving the display, so each frame costs a
    /// cross-GPU copy (§6). Only ever true when the display's own GPU has no
    /// encoder at all.
    bool crossGpuCopy = false;

    /// No GPU in this machine could encode, so `encoder` names a machine-level
    /// fallback (Capabilities::fallbacks) rather than something on `gpu`.
    ///
    /// `gpu` still points at the display's own adapter when there is one: it is
    /// what capture and colour conversion run on, and on a Media Foundation
    /// hardware transform it is also where the frames stay. Only the encoder
    /// moved. Never true while any GPU can encode.
    bool fallbackEncoder = false;

    /// True when that fallback is really running on the CPU, rather than on
    /// fixed-function silicon the OS lent us without a vendor SDK. The one thing
    /// the difference changes downstream is what a session may promise: a CPU
    /// encoder is the case that has to watch whether it is keeping up.
    bool cpuEncoder = false;
};

/// Resolve a SessionConfig against what the machine has.
///
/// Pure: no OS calls, no allocation beyond the error string, no state. That is
/// what lets the whole "which GPU, which encoder, which codec" policy — the
/// part users would otherwise have to configure — be unit-tested on any machine
/// including CI, with no GPU present.
///
/// Returns false and fills `error` when the display does not exist, or when the
/// client and the machine share no codec at all.
bool select(const Capabilities& caps, const SessionConfig& config, Selection& out,
            std::string& error);

/// A frame size and a display size, in pixels.
struct FrameSize
{
    int width = 0;
    int height = 0;
};

/// The frame a display of @p display should be streamed at, given the frame
/// the client asked for: the display's shape at the frame's height, width kept
/// even. The frame is returned untouched when its shape is already within 0.5%
/// of the display's, so a client's even-width rounding is never fought over,
/// and when either size is unknown.
///
/// Never larger than the display: a 1440p request of a 1080p display streams
/// 1080p, whatever the encoder (see select()). Given the size the client asked
/// for rather than the one the session has now, a display that shrank and grew
/// back returns to that size.
///
/// Pure, shared by select() at the start of a session and by every platform
/// session when the display changes mode under it.
FrameSize frameForDisplay(FrameSize display, FrameSize frame);

} // namespace mw::native
