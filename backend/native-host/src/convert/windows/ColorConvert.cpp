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

// windows.h's min/max macros would eat the std:: ones FrameFit.h and the
// resample geometry use; the audio files do the same.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ColorConvert.h"

#include "ConvertHlsl.h"

#include "../../core/Log.h"
#include "../../platform/macos/FrameFit.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace mw::native::convert {
namespace {

// ── The conversion, in HLSL ─────────────────────────────────────────────────
//
// BT.709, limited ("TV") range — the same colour space the existing pipeline
// already negotiates for SDR streams, so the browser's decoder needs no change
// and a native stream looks identical to a Sunshine one on the same screen.
//
// The vertex shader builds a full-screen triangle from SV_VertexID alone: no
// vertex buffer, no input layout, nothing to bind. A triangle rather than a
// quad because a quad's diagonal makes the GPU shade the seam twice.
//
// The scene itself — resources, cursor, tone map — is ConvertHlsl.h's, shared
// with the D3D12 converter. A pixel shader samples it the ordinary way.
constexpr char kPixelPrelude[] = "#define MW_SAMPLE_SOURCE(uv) Source.Sample(Linear, uv)\n";

constexpr char kPixelEntries[] = R"HLSL(
struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VsOut VsMain(uint id : SV_VertexID)
{
    VsOut o;
    // Two of these three vertices fall outside the viewport; the clip does the
    // rest. Cheaper than a quad and needs no buffer.
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float PsLuma(VsOut i) : SV_TARGET
{
    float3 rgb = Scene(i.uv);
    return dot(rgb, kLuma) * kLumaScale + kLumaBias;
}

// 4:4:4, written as packed AYUV in one draw.
//
// NVENC's AYUV is a 32-bit word with V in the lowest byte, then U, then Y, then
// A — so in memory the bytes are [V][U][Y][A], and an R8G8B8A8 render-target
// view lands them as R=V, G=U, B=Y, A=A. Getting that order wrong produces a
// picture with the colours swapped rather than an error, which is why it is
// spelled out here.
float4 PsPacked444(VsOut i) : SV_TARGET
{
    float3 rgb = Scene(i.uv);
    float  y   = dot(rgb, kLuma);

    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;

    return float4(cr * kChromaScale + kChromaBias,  // R <- V
                  cb * kChromaScale + kChromaBias,  // G <- U
                  y  * kLumaScale   + kLumaBias,    // B <- Y
                  1.0);
}

float2 PsChroma(VsOut i) : SV_TARGET
{
    // Sampling once at the chroma texel's centre lets the sampler average the
    // 2x2 luma neighbourhood for us, which is what 4:2:0 wants anyway.
    float3 rgb = Scene(i.uv);
    float  y   = dot(rgb, kLuma);

    // The BT.709 denominators: 2*(1-Kb) and 2*(1-Kr).
    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;
    return float2(cb, cr) * kChromaScale + kChromaBias;
}

float PsLumaHdr(VsOut i) : SV_TARGET
{
    float3 pq = PqFromLinear(mul(kBt709ToBt2020, SceneHdr(i.uv)));
    float  y  = dot(pq, kLuma2020);
    // Limited range, 10-bit: luma 64..940.
    return P010Code(y, 876.0, 64.0);
}

float2 PsChromaHdr(VsOut i) : SV_TARGET
{
    float3 pq = PqFromLinear(mul(kBt709ToBt2020, SceneHdr(i.uv)));
    float  y  = dot(pq, kLuma2020);

    // The BT.2020 denominators: 2*(1-Kb) and 2*(1-Kr).
    float cb = (pq.b - y) / 1.8814;
    float cr = (pq.r - y) / 1.4746;
    // Limited range, 10-bit: chroma 64..960 around a 512 centre.
    return float2(P010Code(cb, 896.0, 512.0), P010Code(cr, 896.0, 512.0));
}
)HLSL";

// ── The resample pass ───────────────────────────────────────────────────────
//
// Resample() itself is in ConvertHlsl.h, with the why. Here it is one pixel
// shader over a full-screen triangle, per direction.
constexpr char kScalePixelEntries[] = R"HLSL(
struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float4 PsScale(VsOut i) : SV_TARGET
{
    return float4(Resample(i.uv), 1.0);
}
)HLSL";

/// @p scRgbSource picks what the BT.709 entry points read: the 8-bit desktop,
/// or the FP16 one through the tone map. Spelled "0"/"1" rather than left
/// undefined, so the shader's #if never depends on what fxc makes of an
/// unknown name.
bool compile(const char* entryPoint, const char* target, bool scRgbSource, ComPtr<ID3DBlob>& blob,
             std::string& error)
{
    const D3D_SHADER_MACRO defines[] = {{"MW_SCRGB_SOURCE", scRgbSource ? "1" : "0"},
                                        {nullptr, nullptr}};
    const std::string source = std::string(kPixelPrelude) + kSceneHlsl + kPixelEntries;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = ::D3DCompile(source.data(), source.size(), "ColorConvert.hlsl", defines,
                                    nullptr, entryPoint, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                    blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr)) return true;

    error = std::string("could not compile ") + entryPoint;
    if (errors && errors->GetBufferPointer()) {
        error += ": ";
        error += static_cast<const char*>(errors->GetBufferPointer());
    }
    return false;
}

/// One direction of the resample pass. @p length is the source extent along
/// the filtered axis, @p output the picture's extent along it, @p fixed the
/// extent of the other axis (rows for horizontal, columns for vertical).
bool compileScale(bool horizontal, bool decode, int length, int output, int fixed,
                  ComPtr<ID3DBlob>& blob, std::string& error)
{
    const ResampleGeometry geometry(length, output, fixed);
    const D3D_SHADER_MACRO defines[] = {{"MW_HORIZONTAL", horizontal ? "1" : "0"},
                                        {"MW_DECODE", decode ? "1" : "0"},
                                        {"MW_DILATE", geometry.dilate.c_str()},
                                        {"MW_TAPS", geometry.taps.c_str()},
                                        {"MW_LEN", geometry.length.c_str()},
                                        {"MW_FIXED", geometry.fixed.c_str()},
                                        {nullptr, nullptr}};
    const std::string source = std::string(kResampleHlsl) + kScalePixelEntries;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = ::D3DCompile(source.data(), source.size(), "ColorScale.hlsl", defines,
                                    nullptr, "PsScale", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                    blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr)) return true;

    error = std::string("could not compile the ") + (horizontal ? "horizontal" : "vertical") +
            " resample";
    if (errors && errors->GetBufferPointer()) {
        error += ": ";
        error += static_cast<const char*>(errors->GetBufferPointer());
    }
    return false;
}

} // namespace

ColorConvert::~ColorConvert() = default;

bool ColorConvert::init(ID3D11Device* device, DXGI_FORMAT sourceFormat, int sourceWidth,
                        int sourceHeight, int outputWidth, int outputHeight, Chroma chroma,
                        bool hdr, ScaleFilter filter, std::string& error)
{
    if (!device) {
        error = "no D3D11 device";
        return false;
    }

    if (!supportsSource(sourceFormat)) {
        error = "no colour conversion for source format " +
                std::to_string(static_cast<int>(sourceFormat));
        return false;
    }

    // The transfer function is not a preference, it is a property of the bytes
    // that arrived. Running the PQ curve over sRGB bytes produces a picture
    // that is merely wrong — blown out — and a wrong picture is far more
    // expensive to diagnose than a refusal. The other way round is a real
    // case, and has its own path: an SDR stream of an HDR desktop takes the
    // FP16 frames through the tone map. See ToneMapToSdr.
    if (hdr && !isHdrSource(sourceFormat)) {
        error = "HDR was asked for but the display delivers 8-bit SDR frames";
        return false;
    }

    // 10-bit 4:4:4 (Y410) is deliberately absent: no browser displays it. See
    // the header.
    if (hdr && chroma == Chroma::C444) {
        error = "HDR is 4:2:0 only (no browser decodes 10-bit 4:4:4)";
        return false;
    }

    m_Device = device;
    m_Device->GetImmediateContext(m_Context.ReleaseAndGetAddressOf());
    m_Chroma = chroma;
    m_Hdr = hdr;
    m_ToneMap = !hdr && isHdrSource(sourceFormat);
    m_SourceFormat = sourceFormat;
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;

    // NV12 chroma is half resolution in both axes, so an odd dimension has no
    // representation at all. Round down rather than up: growing the image would
    // sample outside the captured area.
    m_OutputWidth = (outputWidth > 0 ? outputWidth : sourceWidth) & ~1;
    m_OutputHeight = (outputHeight > 0 ? outputHeight : sourceHeight) & ~1;
    if (m_OutputWidth <= 0 || m_OutputHeight <= 0) {
        error = "output size is degenerate";
        return false;
    }

    // The resample pass only where there is something to resample: at 1:1
    // the conversion reads the capture as it always did, and pays nothing.
    const bool scaling = m_OutputWidth != m_SourceWidth || m_OutputHeight != m_SourceHeight;
    m_Filter = scaling ? filter : ScaleFilter::Bilinear;
    m_Letterboxed = false;
    m_PictureX = m_PictureY = 0.0f;
    m_PictureWidth = static_cast<float>(m_OutputWidth);
    m_PictureHeight = static_cast<float>(m_OutputHeight);
    if (m_Filter != ScaleFilter::Bilinear) {
        // A source of another shape is fitted between bars, as macOS does
        // (FrameFit.h), rather than stretched as the bilinear path does: the
        // Selector never starts a session this way, so this is a display that
        // changed mode under a session told not to follow it.
        const platform::FrameFit fit =
            platform::frameFit(m_SourceWidth, m_SourceHeight, m_OutputWidth, m_OutputHeight);
        const float w = std::floor(m_SourceWidth * fit.scale + 0.5f);
        const float h = std::floor(m_SourceHeight * fit.scale + 0.5f);
        if (w < m_OutputWidth - 1 || h < m_OutputHeight - 1) {
            m_Letterboxed = true;
            m_PictureWidth = std::max(2.0f, w);
            m_PictureHeight = std::max(2.0f, h);
            m_PictureX = std::floor((m_OutputWidth - m_PictureWidth) / 2.0f);
            m_PictureY = std::floor((m_OutputHeight - m_PictureHeight) / 2.0f);
        }
    }

    if (!createShaders(error)) return false;
    if (!createOutput(error)) return false;
    if (m_Filter != ScaleFilter::Bilinear && !createScaler(error)) return false;

    log::info("[native] colour conversion: " + std::to_string(m_SourceWidth) + "x" +
              std::to_string(m_SourceHeight) +
              (m_Hdr       ? " FP16 scRGB -> "
               : m_ToneMap ? " FP16 scRGB, tone-mapped -> "
                           : " BGRA -> ") +
              std::to_string(m_OutputWidth) + "x" + std::to_string(m_OutputHeight) +
              (m_Hdr                      ? " P010 4:2:0 (BT.2020 PQ, limited)"
               : m_Chroma == Chroma::C444 ? " AYUV 4:4:4 (BT.709 limited)"
                                          : " NV12 4:2:0 (BT.709 limited)") +
              (!scaling                            ? ", 1:1"
               : m_Filter == ScaleFilter::Bilinear ? ", scaled bilinear in the pass"
                                                   : ", scaled Lanczos-2 (linear light)") +
              (m_Letterboxed
                   ? ", letterboxed to " + std::to_string(static_cast<int>(m_PictureWidth)) + "x" +
                         std::to_string(static_cast<int>(m_PictureHeight))
                   : ""));
    return true;
}

bool ColorConvert::dropResample()
{
    if (m_Filter == ScaleFilter::Bilinear || m_Letterboxed) return false;
    // convert() branches on m_Filter alone, so this is the whole switch: the
    // conversion samples the capture again, as init() would have set it up.
    // The scaler's textures go with it — VRAM an iGPU shares with the desktop.
    m_Filter = ScaleFilter::Bilinear;
    m_ScaledView.Reset();
    m_ScaledTarget.Reset();
    m_Scaled.Reset();
    m_ScaledMidView.Reset();
    m_ScaledMidTarget.Reset();
    m_ScaledMid.Reset();
    return true;
}

bool ColorConvert::createScaler(std::string& error)
{
    const int pictureWidth = static_cast<int>(m_PictureWidth);
    const int pictureHeight = static_cast<int>(m_PictureHeight);
    // The 8-bit desktop is decoded to light on the way in; the FP16 paths are
    // light already.
    const bool decode = !m_Hdr && !m_ToneMap;

    ComPtr<ID3DBlob> h, v;
    if (!compileScale(true, decode, m_SourceWidth, pictureWidth, m_SourceHeight, h, error))
        return false;
    if (!compileScale(false, false, m_SourceHeight, pictureHeight, pictureWidth, v, error))
        return false;
    if (FAILED(m_Device->CreatePixelShader(h->GetBufferPointer(), h->GetBufferSize(), nullptr,
                                           m_ScaleHShader.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreatePixelShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr,
                                           m_ScaleVShader.ReleaseAndGetAddressOf()))) {
        error = "could not create the resample shaders";
        return false;
    }

    // The intermediate: picture-wide, source-high, linear light in FP16 —
    // 8 bits of linear would posterise the shadows the sRGB curve spends
    // half its codes on.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(pictureWidth);
    desc.Height = static_cast<UINT>(m_SourceHeight);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, m_ScaledMid.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreateRenderTargetView(m_ScaledMid.Get(), nullptr,
                                                m_ScaledMidTarget.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_ScaledMid.Get(), nullptr,
                                                  m_ScaledMidView.ReleaseAndGetAddressOf()))) {
        error = "could not create the resample intermediate";
        return false;
    }

    // The scaled picture the conversion reads. On the 8-bit path a TYPELESS
    // texture written through an _SRGB view (the encode is free) and read
    // through a UNORM one (the conversion wants the encoded signal, as it
    // had from the capture); the scRGB paths stay FP16 and linear.
    desc.Width = static_cast<UINT>(m_OutputWidth);
    desc.Height = static_cast<UINT>(m_OutputHeight);
    desc.Format = decode ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, m_Scaled.ReleaseAndGetAddressOf()))) {
        error = "could not create the scaled picture";
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv.Format = decode ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    srv.Format = decode ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(m_Device->CreateRenderTargetView(m_Scaled.Get(), &rtv,
                                                m_ScaledTarget.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_Scaled.Get(), &srv,
                                                  m_ScaledView.ReleaseAndGetAddressOf()))) {
        error = "could not view the scaled picture";
        return false;
    }
    return true;
}

void ColorConvert::setSdrWhite(float scRgbWhite)
{
    // Below 1.0 does not exist (the slider starts at 80 nits) and 0 would be a
    // division by zero in the shader: a bad read keeps the default.
    m_SdrWhite = scRgbWhite >= 1.0f ? scRgbWhite : 1.0f;
}

bool ColorConvert::createShaders(std::string& error)
{
    ComPtr<ID3DBlob> vs;
    if (!compile("VsMain", "vs_5_0", m_ToneMap, vs, error)) return false;
    if (FAILED(m_Device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                            m_VertexShader.ReleaseAndGetAddressOf()))) {
        error = "could not create the vertex shader";
        return false;
    }

    // Only the pair this session will actually draw with. The PQ shaders carry
    // two pow() chains and are the slowest of the set to compile; a desktop
    // session has no use for them, and an HDR one has no use for the BT.709
    // matrix. The BT.709 pair reads the source the tone-map flag says.
    const char* lumaEntry = m_Hdr ? "PsLumaHdr" : "PsLuma";
    const char* chromaEntry = m_Hdr ? "PsChromaHdr" : "PsChroma";

    ComPtr<ID3DBlob> luma, chroma;
    if (!compile(lumaEntry, "ps_5_0", m_ToneMap, luma, error)) return false;
    if (!compile(chromaEntry, "ps_5_0", m_ToneMap, chroma, error)) return false;

    auto& lumaShader = m_Hdr ? m_LumaHdrShader : m_LumaShader;
    auto& chromaShader = m_Hdr ? m_ChromaHdrShader : m_ChromaShader;
    if (FAILED(m_Device->CreatePixelShader(luma->GetBufferPointer(), luma->GetBufferSize(), nullptr,
                                           lumaShader.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreatePixelShader(chroma->GetBufferPointer(), chroma->GetBufferSize(),
                                           nullptr, chromaShader.ReleaseAndGetAddressOf()))) {
        error = "could not create the conversion shaders";
        return false;
    }

    if (m_Chroma == Chroma::C444) {
        ComPtr<ID3DBlob> packed;
        if (!compile("PsPacked444", "ps_5_0", m_ToneMap, packed, error)) return false;
        if (FAILED(m_Device->CreatePixelShader(packed->GetBufferPointer(), packed->GetBufferSize(),
                                               nullptr, m_PackedShader.ReleaseAndGetAddressOf()))) {
            error = "could not create the 4:4:4 conversion shader";
            return false;
        }
    }

    // Linear filtering is what makes a downscale and the 4:2:0 chroma average
    // come out of the same sample. CLAMP so an edge texel never wraps.
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_Device->CreateSamplerState(&sampler, m_Sampler.ReleaseAndGetAddressOf()))) {
        error = "could not create the sampler";
        return false;
    }

    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(m_Device->CreateSamplerState(&sampler, m_NearestSampler.ReleaseAndGetAddressOf()))) {
        error = "could not create the point sampler";
        return false;
    }

    // Where to draw the cursor, rewritten every frame. DYNAMIC because that is
    // exactly the access pattern: written by the CPU once per frame, read by
    // the GPU immediately after.
    D3D11_BUFFER_DESC overlay = {};
    overlay.ByteWidth = sizeof(float) * 8; // float4 + float + float + float2 padding
    overlay.Usage = D3D11_USAGE_DYNAMIC;
    overlay.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    overlay.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(
            m_Device->CreateBuffer(&overlay, nullptr, m_OverlayBuffer.ReleaseAndGetAddressOf()))) {
        error = "could not create the cursor constant buffer";
        return false;
    }
    return true;
}

bool ColorConvert::updateCursorResources(const capture::CursorState& cursor, std::string& error)
{
    if (cursor.width <= 0 || cursor.height <= 0) return true;
    if (m_CursorPixels && m_CursorShapeVersion == cursor.shapeVersion) return true;

    const UINT width = static_cast<UINT>(cursor.width);
    const UINT height = static_cast<UINT>(cursor.height);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // IMMUTABLE and recreated per shape rather than DYNAMIC and rewritten: a
    // shape change is rare (the pointer keeps one for thousands of frames) and
    // the sizes differ between shapes anyway, so there is nothing to reuse.
    D3D11_SUBRESOURCE_DATA seed = {};
    seed.pSysMem = cursor.pixels.data();
    seed.SysMemPitch = width * 4;

    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(m_Device->CreateTexture2D(&desc, &seed, m_CursorPixels.ReleaseAndGetAddressOf()))) {
        error = "could not upload the cursor image";
        return false;
    }

    seed.pSysMem = cursor.invert.data();
    seed.SysMemPitch = width;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(m_Device->CreateTexture2D(&desc, &seed, m_CursorInvert.ReleaseAndGetAddressOf()))) {
        error = "could not upload the cursor mask";
        return false;
    }

    if (FAILED(m_Device->CreateShaderResourceView(m_CursorPixels.Get(), nullptr,
                                                  m_CursorPixelsView.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_CursorInvert.Get(), nullptr,
                                                  m_CursorInvertView.ReleaseAndGetAddressOf()))) {
        error = "could not view the cursor textures";
        return false;
    }

    m_CursorShapeVersion = cursor.shapeVersion;
    return true;
}

bool ColorConvert::createOutput(std::string& error)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(m_OutputWidth);
    desc.Height = static_cast<UINT>(m_OutputHeight);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = m_Hdr                      ? DXGI_FORMAT_P010
                  : m_Chroma == Chroma::C444 ? DXGI_FORMAT_AYUV
                                             : DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET so the plane (or packed) views can be written;
    // SHADER_RESOURCE because the encoder registers it as an input surface.
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, m_Output.ReleaseAndGetAddressOf()))) {
        error = m_Hdr ? "this GPU cannot render into a P010 texture (HDR unavailable)"
                : m_Chroma == Chroma::C444
                    ? "this GPU cannot render into an AYUV texture (4:4:4 unavailable)"
                    : "this GPU cannot render into an NV12 texture";
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC view = {};
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    if (m_Chroma == Chroma::C444) {
        // One packed plane: an R8G8B8A8 view writes all four bytes of each
        // AYUV word at once, so 4:4:4 needs a single draw where 4:2:0 needs
        // two. See PsPacked444 for the byte order.
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (FAILED(m_Device->CreateRenderTargetView(m_Output.Get(), &view,
                                                    m_PackedTarget.ReleaseAndGetAddressOf()))) {
            error = "could not view the AYUV surface";
            return false;
        }
        return true;
    }

    // The plane is selected by the view's FORMAT, which is the whole trick:
    // R8 addresses NV12's luma, R8G8 its interleaved chroma at half size. P010
    // is the same shape one notch wider — R16 and R16G16 — because its 10 bits
    // are stored in 16-bit words.
    view.Format = m_Hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    if (FAILED(m_Device->CreateRenderTargetView(m_Output.Get(), &view,
                                                m_LumaTarget.ReleaseAndGetAddressOf()))) {
        error = m_Hdr ? "could not view the P010 luma plane" : "could not view the NV12 luma plane";
        return false;
    }

    view.Format = m_Hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(m_Device->CreateRenderTargetView(m_Output.Get(), &view,
                                                m_ChromaTarget.ReleaseAndGetAddressOf()))) {
        error =
            m_Hdr ? "could not view the P010 chroma plane" : "could not view the NV12 chroma plane";
        return false;
    }
    return true;
}

bool ColorConvert::convert(ID3D11Texture2D* source, const capture::CursorState& cursor,
                           const CursorDraw& draw, std::string& error)
{
    if (!source || !m_Context || !m_Output) {
        error = "colour conversion is not initialized";
        return false;
    }

    if (!updateCursorResources(cursor, error)) return false;

    // Desktop Duplication does not promise the same texture object twice, and a
    // view left pointing at the previous one would sample a frozen image
    // forever — a freeze indistinguishable from a network stall.
    if (source != m_SourceViewFor) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = m_SourceFormat;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(m_Device->CreateShaderResourceView(source, &srv,
                                                      m_SourceView.ReleaseAndGetAddressOf()))) {
            error = "could not view the captured frame";
            return false;
        }
        m_SourceViewFor = source;
    }

    // The resample pass, when there is one: the capture goes through the
    // horizontal filter into the intermediate, the intermediate through the
    // vertical one into the scaled picture, and the conversion below reads
    // THAT at 1:1. Bars, if any, are cleared to black first — the vertical
    // pass only paints the fitted rectangle.
    m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_Context->IASetInputLayout(nullptr);
    m_Context->VSSetShader(m_VertexShader.Get(), nullptr, 0);
    if (m_Filter != ScaleFilter::Bilinear) {
        D3D11_VIEWPORT pass = {};
        pass.Width = m_PictureWidth;
        pass.Height = static_cast<float>(m_SourceHeight);
        pass.MaxDepth = 1.0f;
        ID3D11RenderTargetView* midTarget[] = {m_ScaledMidTarget.Get()};
        ID3D11ShaderResourceView* captured[] = {m_SourceView.Get()};
        m_Context->OMSetRenderTargets(1, midTarget, nullptr);
        m_Context->RSSetViewports(1, &pass);
        m_Context->PSSetShader(m_ScaleHShader.Get(), nullptr, 0);
        m_Context->PSSetShaderResources(0, 1, captured);
        m_Context->Draw(3, 0);

        ID3D11RenderTargetView* scaledTarget[] = {m_ScaledTarget.Get()};
        ID3D11ShaderResourceView* unbind[] = {nullptr};
        ID3D11ShaderResourceView* mid[] = {m_ScaledMidView.Get()};
        m_Context->PSSetShaderResources(0, 1, unbind);
        m_Context->OMSetRenderTargets(1, scaledTarget, nullptr);
        if (m_Letterboxed) {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            m_Context->ClearRenderTargetView(m_ScaledTarget.Get(), black);
        }
        pass.TopLeftX = m_PictureX;
        pass.TopLeftY = m_PictureY;
        pass.Width = m_PictureWidth;
        pass.Height = m_PictureHeight;
        m_Context->RSSetViewports(1, &pass);
        m_Context->PSSetShader(m_ScaleVShader.Get(), nullptr, 0);
        m_Context->PSSetShaderResources(0, 1, mid);
        m_Context->Draw(3, 0);
        m_Context->PSSetShaderResources(0, 1, unbind);
        m_Context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    // The cursor rectangle, expressed in the uv of whatever the conversion
    // samples: the capture, or the scaled picture — the same square when the
    // shapes match, the fitted one between the bars when they do not. Drawing
    // in output pixels instead would misplace the pointer by the scale factor
    // on any stream that is not native resolution.
    const bool drawCursor = cursor.visible && m_CursorPixelsView && cursor.width > 0 &&
                            cursor.height > 0 && m_SourceWidth > 0 && m_SourceHeight > 0;
    const bool resampled = m_Filter != ScaleFilter::Bilinear;
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_Context->Map(m_OverlayBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            error = "could not update the cursor position";
            return false;
        }
        // Magnified around the hotspot, not around the top-left: the pointer has
        // to keep aiming at the same pixel while it grows, or a bigger cursor
        // would also be a cursor that clicks somewhere else.
        const float magnify = draw.magnify > 1.0f ? draw.magnify : 1.0f;
        const float width = static_cast<float>(cursor.width) * magnify;
        const float height = static_cast<float>(cursor.height) * magnify;
        const float left = static_cast<float>(cursor.x + draw.hotspotX) -
                           static_cast<float>(draw.hotspotX) * magnify;
        const float top = static_cast<float>(cursor.y + draw.hotspotY) -
                          static_cast<float>(draw.hotspotY) * magnify;

        float* p = static_cast<float*>(mapped.pData);
        p[0] = left / static_cast<float>(m_SourceWidth);
        p[1] = top / static_cast<float>(m_SourceHeight);
        p[2] = width / static_cast<float>(m_SourceWidth);
        p[3] = height / static_cast<float>(m_SourceHeight);
        if (resampled) {
            // Source uv → scaled-picture uv: the identity unless letterboxed.
            const float sx = m_PictureWidth / static_cast<float>(m_OutputWidth);
            const float sy = m_PictureHeight / static_cast<float>(m_OutputHeight);
            p[0] = m_PictureX / static_cast<float>(m_OutputWidth) + p[0] * sx;
            p[1] = m_PictureY / static_cast<float>(m_OutputHeight) + p[1] * sy;
            p[2] *= sx;
            p[3] *= sy;
        }
        p[4] = drawCursor ? 1.0f : 0.0f;
        p[5] = m_SdrWhite;
        p[6] = p[7] = 0.0f;
        m_Context->Unmap(m_OverlayBuffer.Get(), 0);
    }

    ID3D11ShaderResourceView* views[] = {resampled ? m_ScaledView.Get() : m_SourceView.Get(),
                                         m_CursorPixelsView.Get(), m_CursorInvertView.Get()};
    ID3D11SamplerState* samplers[] = {m_Sampler.Get(), m_NearestSampler.Get()};
    ID3D11Buffer* constants[] = {m_OverlayBuffer.Get()};

    m_Context->PSSetShaderResources(0, 3, views);
    m_Context->PSSetSamplers(0, 2, samplers);
    m_Context->PSSetConstantBuffers(0, 1, constants);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_OutputWidth);
    viewport.Height = static_cast<float>(m_OutputHeight);
    viewport.MaxDepth = 1.0f;

    if (m_Chroma == Chroma::C444) {
        // One draw: luma and both chroma components go out together at full
        // resolution.
        ID3D11RenderTargetView* packedTarget[] = {m_PackedTarget.Get()};
        m_Context->OMSetRenderTargets(1, packedTarget, nullptr);
        m_Context->RSSetViewports(1, &viewport);
        m_Context->PSSetShader(m_PackedShader.Get(), nullptr, 0);
        m_Context->Draw(3, 0);
    } else {
        // Luma: full resolution.
        ID3D11RenderTargetView* lumaTarget[] = {m_LumaTarget.Get()};
        m_Context->OMSetRenderTargets(1, lumaTarget, nullptr);
        m_Context->RSSetViewports(1, &viewport);
        m_Context->PSSetShader(m_Hdr ? m_LumaHdrShader.Get() : m_LumaShader.Get(), nullptr, 0);
        m_Context->Draw(3, 0);

        // Chroma: half resolution, which is what makes this 4:2:0.
        viewport.Width = static_cast<float>(m_OutputWidth / 2);
        viewport.Height = static_cast<float>(m_OutputHeight / 2);

        ID3D11RenderTargetView* chromaTarget[] = {m_ChromaTarget.Get()};
        m_Context->OMSetRenderTargets(1, chromaTarget, nullptr);
        m_Context->RSSetViewports(1, &viewport);
        m_Context->PSSetShader(m_Hdr ? m_ChromaHdrShader.Get() : m_ChromaShader.Get(), nullptr, 0);
        m_Context->Draw(3, 0);
    }

    // Unbind the source before returning: capture is about to release the
    // texture, and leaving it bound to the pipeline would keep it alive and
    // trip the next AcquireNextFrame.
    ID3D11ShaderResourceView* none[] = {nullptr, nullptr, nullptr};
    m_Context->PSSetShaderResources(0, 3, none);
    m_Context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

} // namespace mw::native::convert
