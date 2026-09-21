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

#include "ComputeConvert.h"

#include "../ConvertHlsl.h"

#include "../../../core/Log.h"
#include "../../../platform/macos/FrameFit.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace mw::native::convert {
namespace {

using platform::D3d12Context;

// Compute has no derivatives, so no Sample(): level 0 of a one-mip texture is
// the same fetch.
constexpr char kComputePrelude[] =
    "#define MW_SAMPLE_SOURCE(uv) Source.SampleLevel(Linear, uv, 0)\n";

// ColorConvert's Ps* entry points, one thread per output texel instead of one
// fragment. The uv is the texel's centre, which is what the rasterizer hands a
// pixel shader over a full-target viewport.
constexpr char kComputeEntries[] = R"HLSL(
#if MW_CHROMA
RWTexture2D<float2> Target : register(u0);
#else
RWTexture2D<float>  Target : register(u0);
#endif

[numthreads(8, 8, 1)]
void CsMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(MW_TARGET_W) || id.y >= uint(MW_TARGET_H)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(MW_TARGET_W, MW_TARGET_H);

#if MW_HDR
    float3 pq = PqFromLinear(mul(kBt709ToBt2020, SceneHdr(uv)));
    float  y  = dot(pq, kLuma2020);
#if MW_CHROMA
    float cb = (pq.b - y) / 1.8814;
    float cr = (pq.r - y) / 1.4746;
    Target[id.xy] = float2(P010Code(cb, 896.0, 512.0), P010Code(cr, 896.0, 512.0));
#else
    Target[id.xy] = P010Code(y, 876.0, 64.0);
#endif
#else
    float3 rgb = Scene(uv);
    float  y   = dot(rgb, kLuma);
#if MW_CHROMA
    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;
    Target[id.xy] = float2(cb, cr) * kChromaScale + kChromaBias;
#else
    Target[id.xy] = y * kLumaScale + kLumaBias;
#endif
#endif
}
)HLSL";

// The resample pass over its target. The picture's rectangle is the whole
// target except when letterboxed; what is outside it is the bars, written black
// here because there is no ClearRenderTargetView on a compute list.
//
// MW_ENCODE: the 8-bit path stores the scaled picture sRGB-encoded, as the
// conversion expects to find a desktop. D3D11 gets that from an _SRGB render
// target; an unordered-access view has no such format, so it is spelled out.
constexpr char kComputeScaleEntries[] = R"HLSL(
RWTexture2D<float4> Target : register(u0);

float3 LinearToSrgbOut(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)]
void CsMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(MW_TARGET_W) || id.y >= uint(MW_TARGET_H)) return;
    int2 p = int2(id.xy) - int2(MW_PIC_X, MW_PIC_Y);
    if (p.x < 0 || p.y < 0 || p.x >= int(MW_PIC_W) || p.y >= int(MW_PIC_H)) {
        Target[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float3 c = Resample((float2(p) + 0.5) / float2(MW_PIC_W, MW_PIC_H));
#if MW_ENCODE
    c = LinearToSrgbOut(saturate(c));
#endif
    Target[id.xy] = float4(c, 1.0);
}
)HLSL";

enum Slot : UINT
{
    kSlotSource = 0,
    kSlotMid,
    kSlotScaled,
    kSlotCursorPixels,
    kSlotCursorInvert, // must follow kSlotCursorPixels: one table of two
    kSlotMidTarget,
    kSlotScaledTarget,
    kSlotLumaTarget,
    kSlotChromaTarget,
    kSlotCount
};

enum RootParameter : UINT
{
    kRootOverlay = 0, // b0, as root constants: eight floats
    kRootSource,      // t0
    kRootCursor,      // t1, t2
    kRootTarget,      // u0
};

constexpr UINT kThreads = 8;

UINT groups(int extent)
{
    return (static_cast<UINT>(extent) + kThreads - 1) / kThreads;
}

bool compileCompute(const std::string& source, const char* name,
                    const std::vector<std::pair<std::string, std::string>>& macros,
                    ComPtr<ID3DBlob>& blob, std::string& error)
{
    std::vector<D3D_SHADER_MACRO> defines;
    for (const auto& macro : macros)
        defines.push_back({macro.first.c_str(), macro.second.c_str()});
    defines.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = ::D3DCompile(source.data(), source.size(), name, defines.data(), nullptr,
                                    "CsMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                    blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr)) return true;
    error = std::string("could not compile ") + name;
    if (errors && errors->GetBufferPointer()) {
        error += ": ";
        error += static_cast<const char*>(errors->GetBufferPointer());
    }
    return false;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

} // namespace

D3D12_CPU_DESCRIPTOR_HANDLE ComputeConvert::cpuSlot(UINT slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Descriptors->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * m_DescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE ComputeConvert::gpuSlot(UINT slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_Descriptors->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(slot) * m_DescriptorSize;
    return handle;
}

bool ComputeConvert::texture(UINT width, UINT height, DXGI_FORMAT format,
                             D3D12_RESOURCE_FLAGS flags, D3D12_HEAP_FLAGS heapFlags,
                             D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return SUCCEEDED(m_Context->device()->CreateCommittedResource(
        &heap, heapFlags, &desc, state, nullptr, IID_PPV_ARGS(out.ReleaseAndGetAddressOf())));
}

bool ComputeConvert::init(D3d12Context& context, DXGI_FORMAT sourceFormat, int sourceWidth,
                          int sourceHeight, int outputWidth, int outputHeight, bool hdr,
                          ScaleFilter filter, ID3D11Device* consumer, std::string& error)
{
    if (!context.device()) {
        error = "no D3D12 device";
        return false;
    }
    const bool fp16 = sourceFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!fp16 && sourceFormat != DXGI_FORMAT_B8G8R8A8_UNORM &&
        sourceFormat != DXGI_FORMAT_R8G8B8A8_UNORM) {
        error = "no colour conversion for source format " +
                std::to_string(static_cast<int>(sourceFormat));
        return false;
    }
    // As ColorConvert: the transfer function is a property of the bytes that
    // arrived, and PQ over sRGB bytes is a wrong picture, not an error.
    if (hdr && !fp16) {
        error = "HDR was asked for but the display delivers 8-bit SDR frames";
        return false;
    }

    m_Context = &context;
    m_Hdr = hdr;
    m_ToneMap = !hdr && fp16;
    m_SourceFormat = sourceFormat;
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    m_OutputWidth = (outputWidth > 0 ? outputWidth : sourceWidth) & ~1;
    m_OutputHeight = (outputHeight > 0 ? outputHeight : sourceHeight) & ~1;
    if (m_OutputWidth <= 0 || m_OutputHeight <= 0) {
        error = "output size is degenerate";
        return false;
    }

    // The same geometry as ColorConvert::init, to the pixel: the resample pass
    // only where something is resampled, and a source of another shape fitted
    // between bars.
    const bool scaling = m_OutputWidth != m_SourceWidth || m_OutputHeight != m_SourceHeight;
    m_Filter = scaling ? filter : ScaleFilter::Bilinear;
    m_Letterboxed = false;
    m_PictureX = m_PictureY = 0;
    m_PictureWidth = m_OutputWidth;
    m_PictureHeight = m_OutputHeight;
    if (m_Filter != ScaleFilter::Bilinear) {
        const platform::FrameFit fit =
            platform::frameFit(m_SourceWidth, m_SourceHeight, m_OutputWidth, m_OutputHeight);
        const float w = std::floor(m_SourceWidth * fit.scale + 0.5f);
        const float h = std::floor(m_SourceHeight * fit.scale + 0.5f);
        if (w < m_OutputWidth - 1 || h < m_OutputHeight - 1) {
            m_Letterboxed = true;
            m_PictureWidth = static_cast<int>(std::max(2.0f, w));
            m_PictureHeight = static_cast<int>(std::max(2.0f, h));
            m_PictureX = (m_OutputWidth - m_PictureWidth) / 2;
            m_PictureY = (m_OutputHeight - m_PictureHeight) / 2;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kSlotCount;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(context.device()->CreateDescriptorHeap(
            &heap, IID_PPV_ARGS(m_Descriptors.ReleaseAndGetAddressOf())))) {
        error = "could not create the descriptor heap";
        return false;
    }
    m_DescriptorSize =
        context.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_SourceViewFor = nullptr;

    if (!createPipelines(error)) return false;
    if (!createTargets(consumer, error)) return false;
    if (m_Filter != ScaleFilter::Bilinear && !createScalePipelines(error)) return false;
    // The cursor's table has to hold two real textures even before there is a
    // cursor: binding tier 1 hardware reads every descriptor of a bound table.
    if (!createCursor(nullptr, error)) return false;

    log::info("[native] colour conversion (D3D12 compute): " + std::to_string(m_SourceWidth) + "x" +
              std::to_string(m_SourceHeight) +
              (m_Hdr       ? " FP16 scRGB -> "
               : m_ToneMap ? " FP16 scRGB, tone-mapped -> "
                           : " BGRA -> ") +
              std::to_string(m_OutputWidth) + "x" + std::to_string(m_OutputHeight) +
              (m_Hdr ? " P010 4:2:0 (BT.2020 PQ, limited)" : " NV12 4:2:0 (BT.709 limited)") +
              (!scaling                            ? ", 1:1"
               : m_Filter == ScaleFilter::Bilinear ? ", scaled bilinear in the pass"
                                                   : ", scaled Lanczos-2 (linear light)") +
              (m_Letterboxed ? ", letterboxed to " + std::to_string(m_PictureWidth) + "x" +
                                   std::to_string(m_PictureHeight)
                             : "") +
              (m_OutputD3d11 ? ", output shared with D3D11" : ""));
    return true;
}

bool ComputeConvert::createPipelines(std::string& error)
{
    ID3D12Device* device = m_Context->device();

    D3D12_DESCRIPTOR_RANGE source = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE cursor = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, 0};
    D3D12_DESCRIPTOR_RANGE target = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[4] = {};
    parameters[kRootOverlay].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[kRootOverlay].Constants.Num32BitValues = 8;
    const D3D12_DESCRIPTOR_RANGE* ranges[] = {nullptr, &source, &cursor, &target};
    for (UINT i = kRootSource; i <= kRootTarget; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i].DescriptorTable.pDescriptorRanges = ranges[i];
    }

    // s0 linear, s1 point, both clamped — ColorConvert's two.
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    for (UINT i = 0; i < 2; ++i) {
        samplers[i].Filter =
            i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
    }

    D3D12_ROOT_SIGNATURE_DESC signature = {};
    signature.NumParameters = 4;
    signature.pParameters = parameters;
    signature.NumStaticSamplers = 2;
    signature.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(::D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                                             &errors)) ||
        FAILED(
            device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                        IID_PPV_ARGS(m_RootSignature.ReleaseAndGetAddressOf())))) {
        error = "could not create the conversion's root signature";
        return false;
    }

    const std::string sceneSource = std::string(kComputePrelude) + kSceneHlsl + kComputeEntries;
    for (int chroma = 0; chroma < 2; ++chroma) {
        const int width = chroma ? m_OutputWidth / 2 : m_OutputWidth;
        const int height = chroma ? m_OutputHeight / 2 : m_OutputHeight;
        ComPtr<ID3DBlob> shader;
        if (!compileCompute(sceneSource, chroma ? "ComputeChroma.hlsl" : "ComputeLuma.hlsl",
                            {{"MW_SCRGB_SOURCE", m_ToneMap ? "1" : "0"},
                             {"MW_HDR", m_Hdr ? "1" : "0"},
                             {"MW_CHROMA", chroma ? "1" : "0"},
                             {"MW_TARGET_W", std::to_string(width) + ".0"},
                             {"MW_TARGET_H", std::to_string(height) + ".0"}},
                            shader, error))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = m_RootSignature.Get();
        pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        auto& state = chroma ? m_ChromaPipeline : m_LumaPipeline;
        if (FAILED(device->CreateComputePipelineState(
                &pipeline, IID_PPV_ARGS(state.ReleaseAndGetAddressOf())))) {
            error = "could not create the conversion pipeline";
            return false;
        }
    }
    return true;
}

bool ComputeConvert::createScalePipelines(std::string& error)
{
    ID3D12Device* device = m_Context->device();
    // The 8-bit desktop is decoded to light on the way in and re-encoded on the
    // way out; the FP16 paths are light from end to end.
    const bool eightBit = !m_Hdr && !m_ToneMap;
    const std::string source = std::string(kResampleHlsl) + kComputeScaleEntries;

    for (int vertical = 0; vertical < 2; ++vertical) {
        const ResampleGeometry geometry =
            vertical ? ResampleGeometry(m_SourceHeight, m_PictureHeight, m_PictureWidth)
                     : ResampleGeometry(m_SourceWidth, m_PictureWidth, m_SourceHeight);
        const int targetWidth = vertical ? m_OutputWidth : m_PictureWidth;
        const int targetHeight = vertical ? m_OutputHeight : m_SourceHeight;
        ComPtr<ID3DBlob> shader;
        if (!compileCompute(
                source, vertical ? "ComputeScaleV.hlsl" : "ComputeScaleH.hlsl",
                {{"MW_HORIZONTAL", vertical ? "0" : "1"},
                 {"MW_DECODE", !vertical && eightBit ? "1" : "0"},
                 {"MW_ENCODE", vertical && eightBit ? "1" : "0"},
                 {"MW_DILATE", geometry.dilate},
                 {"MW_TAPS", geometry.taps},
                 {"MW_LEN", geometry.length},
                 {"MW_FIXED", geometry.fixed},
                 {"MW_TARGET_W", std::to_string(targetWidth) + ".0"},
                 {"MW_TARGET_H", std::to_string(targetHeight) + ".0"},
                 {"MW_PIC_X", std::to_string(vertical ? m_PictureX : 0)},
                 {"MW_PIC_Y", std::to_string(vertical ? m_PictureY : 0)},
                 {"MW_PIC_W", std::to_string(m_PictureWidth) + ".0"},
                 {"MW_PIC_H", std::to_string(vertical ? m_PictureHeight : m_SourceHeight) + ".0"}},
                shader, error))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = m_RootSignature.Get();
        pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        auto& state = vertical ? m_ScaleVPipeline : m_ScaleHPipeline;
        if (FAILED(device->CreateComputePipelineState(
                &pipeline, IID_PPV_ARGS(state.ReleaseAndGetAddressOf())))) {
            error = "could not create the resample pipeline";
            return false;
        }
    }

    // The intermediate in FP16 — 8 bits of linear light would posterise the
    // shadows — and the scaled picture as the conversion wants to read it.
    const DXGI_FORMAT scaledFormat =
        eightBit ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!texture(static_cast<UINT>(m_PictureWidth), static_cast<UINT>(m_SourceHeight),
                 DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                 D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_ScaledMid) ||
        !texture(static_cast<UINT>(m_OutputWidth), static_cast<UINT>(m_OutputHeight), scaledFormat,
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_Scaled)) {
        error = "could not create the resample textures";
        return false;
    }
    device->CreateShaderResourceView(m_ScaledMid.Get(), nullptr, cpuSlot(kSlotMid));
    device->CreateShaderResourceView(m_Scaled.Get(), nullptr, cpuSlot(kSlotScaled));
    device->CreateUnorderedAccessView(m_ScaledMid.Get(), nullptr, nullptr, cpuSlot(kSlotMidTarget));
    device->CreateUnorderedAccessView(m_Scaled.Get(), nullptr, nullptr, cpuSlot(kSlotScaledTarget));
    return true;
}

bool ComputeConvert::createTargets(ID3D11Device* consumer, std::string& error)
{
    ID3D12Device* device = m_Context->device();
    const DXGI_FORMAT format = m_Hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    // Shared with D3D11, it has to look like something D3D11 could have made:
    // on the Arc, the RTX 5060 Ti and the AMD iGPU alike (probed 21/09/2026),
    // OpenSharedResource1 refuses an NV12 or P010 texture — E_INVALIDARG —
    // unless it is a render target with simultaneous access, and accepts
    // unordered access on top of those two.
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (consumer)
        flags |=
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (!texture(static_cast<UINT>(m_OutputWidth), static_cast<UINT>(m_OutputHeight), format, flags,
                 consumer ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE,
                 D3D12_RESOURCE_STATE_COMMON, m_Output)) {
        error = m_Hdr ? "this GPU cannot write a P010 texture from compute (HDR unavailable)"
                      : "this GPU cannot write an NV12 texture from compute";
        return false;
    }

    // The plane is the view's PlaneSlice, and its layout the view's format: R8
    // and R8G8 for NV12, the 16-bit pair for P010's 10 bits in 16-bit words.
    D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    view.Format = m_Hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    device->CreateUnorderedAccessView(m_Output.Get(), nullptr, &view, cpuSlot(kSlotLumaTarget));
    view.Format = m_Hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    view.Texture2D.PlaneSlice = 1;
    device->CreateUnorderedAccessView(m_Output.Get(), nullptr, &view, cpuSlot(kSlotChromaTarget));

    m_OutputD3d11.Reset();
    if (!consumer) return true;

    HANDLE handle = nullptr;
    HRESULT hr = device->CreateSharedHandle(m_Output.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    ComPtr<ID3D11Device1> consumer1;
    if (SUCCEEDED(hr)) hr = consumer->QueryInterface(IID_PPV_ARGS(&consumer1));
    if (SUCCEEDED(hr))
        hr = consumer1->OpenSharedResource1(handle,
                                            IID_PPV_ARGS(m_OutputD3d11.ReleaseAndGetAddressOf()));
    if (handle) ::CloseHandle(handle);
    if (FAILED(hr)) {
        error = "the converted picture cannot be shared with the D3D11 encoder (" +
                D3d12Context::hresultToString(hr) + ")";
        return false;
    }
    return true;
}

bool ComputeConvert::createCursor(const capture::CursorState* cursor, std::string& error)
{
    ID3D12Device* device = m_Context->device();
    const UINT width = cursor ? static_cast<UINT>(cursor->width) : 1;
    const UINT height = cursor ? static_cast<UINT>(cursor->height) : 1;
    // The placeholder is never sampled (CursorEnabled stays 0), so it needs no
    // content and goes straight to the state the shader reads it in.
    const D3D12_RESOURCE_STATES state =
        cursor ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!texture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                 D3D12_HEAP_FLAG_NONE, state, m_CursorPixels) ||
        !texture(width, height, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                 D3D12_HEAP_FLAG_NONE, state, m_CursorInvert)) {
        error = "could not create the cursor textures";
        return false;
    }
    device->CreateShaderResourceView(m_CursorPixels.Get(), nullptr, cpuSlot(kSlotCursorPixels));
    device->CreateShaderResourceView(m_CursorInvert.Get(), nullptr, cpuSlot(kSlotCursorInvert));
    m_CursorPending = false;
    if (!cursor) return true;

    // One upload buffer for both: the image, then the mask behind it.
    const D3D12_RESOURCE_DESC pixelsDesc = m_CursorPixels->GetDesc();
    const D3D12_RESOURCE_DESC invertDesc = m_CursorInvert->GetDesc();
    UINT64 pixelsBytes = 0, invertBytes = 0;
    device->GetCopyableFootprints(&pixelsDesc, 0, 1, 0, &m_CursorPixelsFootprint, nullptr, nullptr,
                                  &pixelsBytes);
    const UINT64 invertOffset = (pixelsBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                                ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    device->GetCopyableFootprints(&invertDesc, 0, 1, invertOffset, &m_CursorInvertFootprint,
                                  nullptr, nullptr, &invertBytes);

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = invertOffset + invertBytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uint8_t* mapped = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(m_CursorUpload.ReleaseAndGetAddressOf()))) ||
        FAILED(m_CursorUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
        error = "could not upload the cursor image";
        return false;
    }
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(mapped + m_CursorPixelsFootprint.Offset +
                        static_cast<size_t>(y) * m_CursorPixelsFootprint.Footprint.RowPitch,
                    cursor->pixels.data() + static_cast<size_t>(y) * width * 4, width * 4);
        std::memcpy(mapped + m_CursorInvertFootprint.Offset +
                        static_cast<size_t>(y) * m_CursorInvertFootprint.Footprint.RowPitch,
                    cursor->invert.data() + static_cast<size_t>(y) * width, width);
    }
    m_CursorUpload->Unmap(0, nullptr);
    m_CursorPending = true;
    m_HasCursorShape = true;
    m_CursorShapeVersion = cursor->shapeVersion;
    return true;
}

void ComputeConvert::setSdrWhite(float scRgbWhite)
{
    m_SdrWhite = scRgbWhite >= 1.0f ? scRgbWhite : 1.0f;
}

bool ComputeConvert::dropResample()
{
    if (m_Filter == ScaleFilter::Bilinear || m_Letterboxed) return false;
    // convert() branches on m_Filter alone; the output texture — what the
    // encoder holds — is untouched. Nothing is in flight: convert() waits.
    m_Filter = ScaleFilter::Bilinear;
    m_ScaleHPipeline.Reset();
    m_ScaleVPipeline.Reset();
    m_Scaled.Reset();
    m_ScaledMid.Reset();
    m_PictureX = m_PictureY = 0;
    m_PictureWidth = m_OutputWidth;
    m_PictureHeight = m_OutputHeight;
    return true;
}

bool ComputeConvert::convert(ID3D12Resource* source, const capture::CursorState& cursor,
                             const CursorDraw& draw, std::string& error)
{
    if (!source || !m_Context || !m_Output) {
        error = "colour conversion is not initialized";
        return false;
    }

    const bool hasShape =
        cursor.width > 0 && cursor.height > 0 &&
        cursor.pixels.size() >= static_cast<size_t>(cursor.width) * cursor.height * 4 &&
        cursor.invert.size() >= static_cast<size_t>(cursor.width) * cursor.height;
    if (hasShape && (!m_HasCursorShape || m_CursorShapeVersion != cursor.shapeVersion) &&
        !createCursor(&cursor, error))
        return false;

    // Desktop Duplication does not promise the same surface twice, and a view
    // left on the previous one would show a frozen picture for ever.
    if (source != m_SourceViewFor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = m_SourceFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        m_Context->device()->CreateShaderResourceView(source, &view, cpuSlot(kSlotSource));
        m_SourceViewFor = source;
    }

    ID3D12GraphicsCommandList* list = m_Context->begin(error);
    if (!list) return false;

    ID3D12DescriptorHeap* heaps[] = {m_Descriptors.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_RootSignature.Get());

    if (m_CursorPending) {
        D3D12_TEXTURE_COPY_LOCATION from = {m_CursorUpload.Get(),
                                            D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        D3D12_TEXTURE_COPY_LOCATION to = {m_CursorPixels.Get(),
                                          D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        from.PlacedFootprint = m_CursorPixelsFootprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        to.pResource = m_CursorInvert.Get();
        from.PlacedFootprint = m_CursorInvertFootprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        const D3D12_RESOURCE_BARRIER ready[] = {
            transition(m_CursorPixels.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(m_CursorInvert.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2, ready);
        m_CursorPending = false;
    }

    // The cursor rectangle in the uv of whatever the conversion samples — the
    // capture, or the scaled picture. ColorConvert::convert, to the letter.
    const bool resampled = m_Filter != ScaleFilter::Bilinear;
    const bool drawCursor =
        cursor.visible && m_HasCursorShape && hasShape && m_SourceWidth > 0 && m_SourceHeight > 0;
    float overlay[8] = {};
    {
        const float magnify = draw.magnify > 1.0f ? draw.magnify : 1.0f;
        const float width = static_cast<float>(cursor.width) * magnify;
        const float height = static_cast<float>(cursor.height) * magnify;
        const float left = static_cast<float>(cursor.x + draw.hotspotX) -
                           static_cast<float>(draw.hotspotX) * magnify;
        const float top = static_cast<float>(cursor.y + draw.hotspotY) -
                          static_cast<float>(draw.hotspotY) * magnify;
        overlay[0] = left / static_cast<float>(m_SourceWidth);
        overlay[1] = top / static_cast<float>(m_SourceHeight);
        overlay[2] = width / static_cast<float>(m_SourceWidth);
        overlay[3] = height / static_cast<float>(m_SourceHeight);
        if (resampled) {
            const float sx = static_cast<float>(m_PictureWidth) / static_cast<float>(m_OutputWidth);
            const float sy =
                static_cast<float>(m_PictureHeight) / static_cast<float>(m_OutputHeight);
            overlay[0] = static_cast<float>(m_PictureX) / static_cast<float>(m_OutputWidth) +
                         overlay[0] * sx;
            overlay[1] = static_cast<float>(m_PictureY) / static_cast<float>(m_OutputHeight) +
                         overlay[1] * sy;
            overlay[2] *= sx;
            overlay[3] *= sy;
        }
        overlay[4] = drawCursor ? 1.0f : 0.0f;
        overlay[5] = m_SdrWhite;
    }
    list->SetComputeRoot32BitConstants(kRootOverlay, 8, overlay, 0);
    list->SetComputeRootDescriptorTable(kRootCursor, gpuSlot(kSlotCursorPixels));

    // The captured surface arrives in COMMON and goes back to it: it is the
    // desktop compositor's, and D3D11 knows nothing of D3D12's states.
    D3D12_RESOURCE_BARRIER barrier = transition(source, D3D12_RESOURCE_STATE_COMMON,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &barrier);

    if (resampled) {
        // Horizontal into the intermediate, vertical into the scaled picture,
        // which the conversion below then reads 1:1.
        list->SetPipelineState(m_ScaleHPipeline.Get());
        list->SetComputeRootDescriptorTable(kRootSource, gpuSlot(kSlotSource));
        list->SetComputeRootDescriptorTable(kRootTarget, gpuSlot(kSlotMidTarget));
        list->Dispatch(groups(m_PictureWidth), groups(m_SourceHeight), 1);

        barrier = transition(m_ScaledMid.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &barrier);
        list->SetPipelineState(m_ScaleVPipeline.Get());
        list->SetComputeRootDescriptorTable(kRootSource, gpuSlot(kSlotMid));
        list->SetComputeRootDescriptorTable(kRootTarget, gpuSlot(kSlotScaledTarget));
        list->Dispatch(groups(m_OutputWidth), groups(m_OutputHeight), 1);

        const D3D12_RESOURCE_BARRIER next[] = {
            transition(m_ScaledMid.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition(m_Scaled.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2, next);
    }

    barrier = transition(m_Output.Get(), D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &barrier);
    list->SetComputeRootDescriptorTable(kRootSource,
                                        gpuSlot(resampled ? kSlotScaled : kSlotSource));
    list->SetPipelineState(m_LumaPipeline.Get());
    list->SetComputeRootDescriptorTable(kRootTarget, gpuSlot(kSlotLumaTarget));
    list->Dispatch(groups(m_OutputWidth), groups(m_OutputHeight), 1);
    // Chroma: half resolution, which is what makes this 4:2:0.
    list->SetPipelineState(m_ChromaPipeline.Get());
    list->SetComputeRootDescriptorTable(kRootTarget, gpuSlot(kSlotChromaTarget));
    list->Dispatch(groups(m_OutputWidth / 2), groups(m_OutputHeight / 2), 1);

    const D3D12_RESOURCE_BARRIER done[] = {
        transition(m_Output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COMMON),
        transition(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON)};
    list->ResourceBarrier(2, done);
    if (resampled) {
        barrier = transition(m_Scaled.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &barrier);
    }

    return m_Context->submitAndWait(error);
}

} // namespace mw::native::convert
