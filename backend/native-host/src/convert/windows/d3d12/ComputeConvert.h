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

#include "../../../platform/windows/d3d12/D3d12Context.h"

#include "../../../capture/CaptureTypes.h"
#include "../../CursorDraw.h"
#include "../../ScaleFilter.h"

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <string>

namespace mw::native::convert {

/// ColorConvert's work, dispatched on a D3D12 compute queue instead of drawn on
/// the D3D11 device's only queue — see D3d12Context for why that matters.
///
/// ── The same picture ────────────────────────────────────────────────────────
///
/// Not a reimplementation: the HLSL is ConvertHlsl.h's, the text ColorConvert
/// compiles, with compute entry points where that one has pixel shaders. Same
/// Lanczos-2 resample in linear light, same cursor, same tone map, same BT.709
/// and BT.2020 PQ matrices. A viewer must not be able to tell which pipeline
/// carried the stream, and tests/test_compute_convert.cpp holds the two to
/// within one code of each other.
///
/// Two differences that D3D12 forces, neither visible:
///  - NV12's planes are written through unordered-access views of a plane
///    slice, which is exactly what D3D11 lacks (ColorConvert.h, "why a render
///    pass");
///  - there is no _SRGB unordered-access view, so the resample pass encodes its
///    8-bit result with the sRGB formula where D3D11 lets the render target do
///    it.
///
/// ── 4:2:0 only ──────────────────────────────────────────────────────────────
///
/// No D3D12 encoder on any of the three vendors takes AYUV (probed 21/09/2026),
/// so a 4:4:4 session has nothing to gain here and stays on the D3D11 pipeline.
///
/// ── Who reads the result ────────────────────────────────────────────────────
///
/// Either a D3D12 encoder, straight from output(); or — the "hybrid" pipeline —
/// a D3D11 one (oneVPL, NVENC, AMF as they are today), through outputD3d11():
/// the same VRAM, opened on the encoder's device through a shared handle. No
/// cross-API fence is needed for that: convert() returns once the GPU is done.
class ComputeConvert
{
public:
    ComputeConvert() = default;
    ~ComputeConvert() = default;

    ComputeConvert(const ComputeConvert&) = delete;
    ComputeConvert& operator=(const ComputeConvert&) = delete;

    using ScaleFilter = convert::ScaleFilter;

    /// As ColorConvert::init, 4:2:0 only. @p context must outlive this object.
    /// With @p consumer, the output is created shareable and opened on that
    /// D3D11 device — which has to be on the same adapter.
    bool init(platform::D3d12Context& context, DXGI_FORMAT sourceFormat, int sourceWidth,
              int sourceHeight, int outputWidth, int outputHeight, bool hdr, ScaleFilter filter,
              ID3D11Device* consumer, std::string& error);

    ScaleFilter scaleFilter() const { return m_Filter; }
    bool dropResample();
    bool letterboxed() const { return m_Letterboxed; }
    bool hdr() const { return m_Hdr; }
    bool toneMapsToSdr() const { return m_ToneMap; }
    bool scRgbSource() const { return m_Hdr || m_ToneMap; }
    void setSdrWhite(float scRgbWhite);
    float sdrWhite() const { return m_SdrWhite; }

    /// Convert one frame, and return when the GPU has finished it: @p source
    /// may be released, and the output read, the moment this returns.
    bool convert(ID3D12Resource* source, const capture::CursorState& cursor, const CursorDraw& draw,
                 std::string& error);

    /// NV12 or P010, in D3D12_RESOURCE_STATE_COMMON between two convert().
    ID3D12Resource* output() const { return m_Output.Get(); }
    /// The same texture on the consumer's device; null without one.
    ID3D11Texture2D* outputD3d11() const { return m_OutputD3d11.Get(); }

    int outputWidth() const { return m_OutputWidth; }
    int outputHeight() const { return m_OutputHeight; }

private:
    bool createPipelines(std::string& error);
    bool createScalePipelines(std::string& error);
    bool createTargets(ID3D11Device* consumer, std::string& error);
    bool createCursor(const capture::CursorState* cursor, std::string& error);
    bool texture(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                 D3D12_HEAP_FLAGS heapFlags, D3D12_RESOURCE_STATES state,
                 Microsoft::WRL::ComPtr<ID3D12Resource>& out);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSlot(UINT slot) const;

    platform::D3d12Context* m_Context = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_LumaPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ChromaPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ScaleHPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ScaleVPipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_Descriptors;
    UINT m_DescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_Output;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_OutputD3d11;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ScaledMid;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Scaled;

    /// The cursor's two textures and the buffer they are uploaded from. A new
    /// shape simply replaces them: convert() waits for the GPU, so nothing is
    /// ever in flight between two calls.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_CursorPixels;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_CursorInvert;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_CursorUpload;
    bool m_CursorPending = false; // uploaded into m_CursorUpload, not copied yet
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_CursorPixelsFootprint = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_CursorInvertFootprint = {};
    uint64_t m_CursorShapeVersion = 0;
    bool m_HasCursorShape = false;

    ID3D12Resource* m_SourceViewFor = nullptr;

    ScaleFilter m_Filter = ScaleFilter::Bilinear;
    bool m_Letterboxed = false;
    int m_PictureX = 0;
    int m_PictureY = 0;
    int m_PictureWidth = 0;
    int m_PictureHeight = 0;

    bool m_Hdr = false;
    bool m_ToneMap = false;
    float m_SdrWhite = 1.0f;
    DXGI_FORMAT m_SourceFormat = DXGI_FORMAT_UNKNOWN;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_OutputWidth = 0;
    int m_OutputHeight = 0;
};

} // namespace mw::native::convert
