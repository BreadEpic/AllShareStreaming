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

#include "d3d12/ComputeConvert.h"

#include "ColorConvert.h"

#include <memory>
#include <string>

namespace mw::native::convert {

/// The session's converter: ColorConvert, or ComputeConvert behind the same
/// calls, so the session's loop reads the same whichever carries the picture.
///
/// ── The rollback is built in ────────────────────────────────────────────────
///
/// The D3D12 converter is asked for, never assumed. init() falls back to the
/// D3D11 one when it cannot be had (no D3D12 on the GPU, 4:4:4, a driver that
/// will not share the output), and so does convert() in the middle of a stream
/// — a captured surface that will not open in D3D12, a compute queue that stops
/// answering. Either way the log says why, the stream goes on, and the encoder
/// simply sees another texture in output(): every Windows encoder here takes
/// its input per frame, or re-registers it when the pointer changes.
///
/// ── Textures the session makes itself ───────────────────────────────────────
///
/// Desktop Duplication's surfaces are shareable as they come. The two the
/// session creates — the desktop copy a pointer-only frame is redrawn from, the
/// black picture of a display that went away — are not, unless created with
/// sessionTextureMiscFlags(). And they are WRITTEN by the D3D11 device, on the
/// very queue this path exists to stay out of: after writing one, the session
/// calls wroteOnD3d11(), and the compute queue waits — on the GPU, not the CPU —
/// for that write before it reads the texture. Without it a pointer-only frame
/// under a saturated GPU would be converted from a copy still sitting in the 3D
/// queue.
class PictureConverter
{
public:
    using Chroma = ColorConvert::Chroma;
    using ScaleFilter = convert::ScaleFilter;

    /// @p compute asks for the D3D12 converter; the rest is ColorConvert::init.
    bool init(bool compute, ID3D11Device* device, DXGI_FORMAT sourceFormat, int sourceWidth,
              int sourceHeight, int outputWidth, int outputHeight, Chroma chroma, bool hdr,
              ScaleFilter filter, std::string& error);

    /// Whether the conversion runs on the D3D12 compute queue right now.
    bool onCompute() const { return m_Compute != nullptr; }

    UINT sessionTextureMiscFlags() const;
    void wroteOnD3d11(ID3D11DeviceContext* context);

    bool convert(ID3D11Texture2D* source, const capture::CursorState& cursor,
                 const CursorDraw& draw, std::string& error);

    /// NV12, P010 or AYUV on the D3D11 device, whoever wrote it.
    ID3D11Texture2D* output() const;

    /// The D3D12 side, for an encoder that lives there. Null on the D3D11 path.
    platform::D3d12Context* context12() const { return m_Compute ? m_Context.get() : nullptr; }
    ID3D12Resource* output12() const { return m_Compute ? m_Compute->output() : nullptr; }

    int outputWidth() const;
    int outputHeight() const;
    ScaleFilter scaleFilter() const;
    bool dropResample();
    bool letterboxed() const;
    bool toneMapsToSdr() const;
    bool scRgbSource() const;
    void setSdrWhite(float scRgbWhite);

private:
    bool initD3d11(std::string& error);
    void fallBack(const std::string& reason);

    std::unique_ptr<ColorConvert> m_D3d11;
    std::unique_ptr<platform::D3d12Context> m_Context;
    std::unique_ptr<ComputeConvert> m_Compute;

    // What init() was given, for a fallback in the middle of a stream.
    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    DXGI_FORMAT m_SourceFormat = DXGI_FORMAT_UNKNOWN;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_OutputWidth = 0;
    int m_OutputHeight = 0;
    Chroma m_Chroma = Chroma::C420;
    bool m_Hdr = false;
    ScaleFilter m_Filter = ScaleFilter::Bilinear;
    float m_SdrWhite = 1.0f;
};

} // namespace mw::native::convert
