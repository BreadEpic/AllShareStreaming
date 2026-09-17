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

#include "Pass.h"

#include "Image.h"

#include <d3dcompiler.h>

// The FSR constant setup, on the CPU: the header computes the constant
// buffer when A_CPU is defined.
#define A_CPU 1
#include "ffx/ffx_a.h"
#include "ffx/ffx_fsr1.h"

#include "nis/NIS_Config.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace bench {

namespace {

std::string hex(HRESULT hr)
{
    std::ostringstream s;
    s << "0x" << std::hex << static_cast<unsigned long>(hr);
    return s.str();
}

/// The pixel-shader candidates' constant buffer (common.hlsli `Params`).
struct Params
{
    float srcSize[2];
    float invSrcSize[2];
    float dstSize[2];
    float invDstSize[2];
    float scale[2];
    float invScale[2];
    float pad[4];
};

bool createConstants(ID3D11Device* device, const void* data, size_t size, ComPtr<ID3D11Buffer>& out,
                     std::string& reason)
{
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>((size + 15) & ~size_t(15));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    std::vector<uint8_t> padded(desc.ByteWidth, 0);
    std::memcpy(padded.data(), data, size);
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = padded.data();
    if (FAILED(device->CreateBuffer(&desc, &init, out.ReleaseAndGetAddressOf()))) {
        reason = "could not create a constant buffer";
        return false;
    }
    return true;
}

} // namespace

// ── SourceSet ───────────────────────────────────────────────────────────────

void SourceSet::init(ID3D11Device* device, Range range, const ImageF* encoded,
                     const ImageF* encodedShifted, const ImageF* linear,
                     const ImageF* linearShifted)
{
    m_Device = device;
    m_Range = range;
    m_Encoded = encoded;
    m_EncodedShifted = encodedShifted;
    m_Linear = linear;
    m_LinearShifted = linearShifted;
    m_Cache.clear();
}

const SourceSet::Tex* SourceSet::get(Flavour flavour, bool shifted, bool mips, std::string& error)
{
    const int key = static_cast<int>(flavour) * 4 + (shifted ? 2 : 0) + (mips ? 1 : 0);
    auto found = m_Cache.find(key);
    if (found != m_Cache.end()) return &found->second;

    const ImageF* encoded = shifted ? m_EncodedShifted : m_Encoded;
    const ImageF* linear = shifted ? m_LinearShifted : m_Linear;

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT viewSrgb = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t> bytes;
    UINT pitch = 0;
    switch (flavour) {
    case Flavour::Srgb8: {
        if (m_Range != Range::Sdr) {
            error = "the 8-bit source only exists for SDR";
            return nullptr;
        }
        format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        view = DXGI_FORMAT_R8G8B8A8_UNORM;
        viewSrgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        bytes = image::toRgba8(*encoded);
        pitch = encoded->w * 4;
        break;
    }
    case Flavour::Rgba8: {
        if (m_Range != Range::Sdr) {
            error = "the 8-bit source only exists for SDR";
            return nullptr;
        }
        format = view = DXGI_FORMAT_R8G8B8A8_UNORM;
        bytes = image::toRgba8(*encoded);
        pitch = encoded->w * 4;
        break;
    }
    case Flavour::Half16: {
        if (!linear) {
            error = "no linear picture for this range";
            return nullptr;
        }
        format = view = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const std::vector<uint16_t> half = image::toRgba16Half(*linear);
        bytes.resize(half.size() * 2);
        std::memcpy(bytes.data(), half.data(), bytes.size());
        pitch = linear->w * 8;
        break;
    }
    case Flavour::Pq16: {
        if (m_Range != Range::Hdr) {
            error = "the PQ source only exists for HDR";
            return nullptr;
        }
        format = view = DXGI_FORMAT_R16G16B16A16_UNORM;
        const std::vector<uint16_t> u16 = image::toRgba16Unorm(*encoded);
        bytes.resize(u16.size() * 2);
        std::memcpy(bytes.data(), u16.data(), bytes.size());
        pitch = encoded->w * 8;
        break;
    }
    }

    Tex tex;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = encoded->w;
    desc.Height = encoded->h;
    desc.MipLevels = mips ? 0 : 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // The video processor wants its input view on a render-target-capable
    // texture (E_INVALIDARG otherwise, NVIDIA 17/09/2026); harmless for the rest.
    if (flavour == Flavour::Rgba8 || flavour == Flavour::Half16)
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (mips) {
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }
    if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, tex.texture.ReleaseAndGetAddressOf()))) {
        error = "could not create a source texture";
        return nullptr;
    }
    ComPtr<ID3D11DeviceContext> context;
    m_Device->GetImmediateContext(context.GetAddressOf());
    context->UpdateSubresource(tex.texture.Get(), 0, nullptr, bytes.data(), pitch, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = mips ? static_cast<UINT>(-1) : 1;
    srv.Format = view;
    if (FAILED(m_Device->CreateShaderResourceView(tex.texture.Get(), &srv,
                                                  tex.srv.ReleaseAndGetAddressOf()))) {
        error = "could not create a source view";
        return nullptr;
    }
    if (viewSrgb != DXGI_FORMAT_UNKNOWN) {
        srv.Format = viewSrgb;
        if (FAILED(m_Device->CreateShaderResourceView(tex.texture.Get(), &srv,
                                                      tex.srvSrgb.ReleaseAndGetAddressOf()))) {
            error = "could not create the sRGB source view";
            return nullptr;
        }
    }
    return &(m_Cache[key] = std::move(tex));
}

// ── Pass ────────────────────────────────────────────────────────────────────

bool Pass::compile(const std::wstring& path, const char* entry, const char* target,
                   const std::vector<std::pair<std::string, std::string>>& defines,
                   ComPtr<ID3DBlob>& blob, std::string& reason)
{
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& d : defines)
        macros.push_back({d.first.c_str(), d.second.c_str()});
    macros.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = ::D3DCompileFromFile(
        path.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr)) return true;
    reason = std::string("shader ") + entry + " did not compile (" + hex(hr) + ")";
    if (errors && errors->GetBufferPointer()) {
        reason += ": ";
        reason += static_cast<const char*>(errors->GetBufferPointer());
    }
    return false;
}

bool Pass::createOutput(DXGI_FORMAT format, DXGI_FORMAT viewFormat, bool uav, std::string& reason)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_Dst.w;
    desc.Height = m_Dst.h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                     (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, m_Output.ReleaseAndGetAddressOf()))) {
        reason = "could not create the output texture";
        return false;
    }
    m_OutputFormat = format;

    D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = viewFormat;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(m_Device->CreateRenderTargetView(m_Output.Get(), &rtv,
                                                m_OutputRtv.ReleaseAndGetAddressOf()))) {
        reason = "could not create the output's render-target view";
        return false;
    }
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = viewFormat;
        u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(m_Device->CreateUnorderedAccessView(m_Output.Get(), &u,
                                                       m_OutputUav.ReleaseAndGetAddressOf()))) {
            reason = "could not create the output's UAV";
            return false;
        }
    }

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, m_Staging.ReleaseAndGetAddressOf()))) {
        reason = "could not create the staging texture";
        return false;
    }
    return true;
}

bool Pass::init(const Gpu& gpu, const Variant& variant, const Case& c, Range range,
                SourceSet& sources, const std::wstring& shaderDir, std::string& reason)
{
    m_Device = gpu.device;
    m_Context = gpu.context;
    m_Variant = variant;
    m_Range = range;
    m_Src = c.src;
    m_Dst = c.dst;
    m_Unsupported = false;
    m_Notes.clear();

    if (range == Range::Sdr && !variant.sdr) {
        m_Unsupported = true;
        reason = "not an SDR candidate";
        return false;
    }
    if (range == Range::Hdr && !variant.hdr) {
        m_Unsupported = true;
        reason = "not an HDR candidate";
        return false;
    }
    if (variant.kind == Kind::VideoProcessor)
        return initVideoProcessor(gpu, variant, c, range, sources, reason);
    return initShader(gpu, variant, c, range, sources, shaderDir, reason);
}

bool Pass::initShader(const Gpu& gpu, const Variant& v, const Case& c, Range range,
                      SourceSet& sources, const std::wstring& shaderDir, std::string& reason)
{
    (void)gpu;
    (void)c;
    // ── Inputs ──
    const bool mips = v.kind == Kind::Mip;
    SourceSet::Flavour flavour;
    if (range == Range::Sdr) {
        flavour = SourceSet::Flavour::Srgb8;
    } else {
        flavour = v.space == Space::Linear ? SourceSet::Flavour::Half16 : SourceSet::Flavour::Pq16;
    }
    for (int i = 0; i < 2; ++i) {
        m_Input[i] = sources.get(flavour, i == 1, mips, reason);
        if (!m_Input[i]) return false;
        m_InputView[i] = (range == Range::Sdr && v.space == Space::Linear) ? m_Input[i]->srvSrgb
                                                                           : m_Input[i]->srv;
    }

    // ── Output ──
    const bool uav = v.kind == Kind::Compute;
    DXGI_FORMAT format;
    DXGI_FORMAT view;
    if (range == Range::Sdr) {
        format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        // A linear-light candidate writes through an sRGB view so the bytes
        // come out encoded, like every other candidate's.
        view =
            v.space == Space::Linear ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (uav && v.space == Space::Linear) {
            m_Unsupported = true;
            reason = "a UAV cannot be sRGB-typed; the compute candidates are perceptual only";
            return false;
        }
    } else {
        format = view = v.space == Space::Linear ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                 : DXGI_FORMAT_R16G16B16A16_UNORM;
    }
    if (!createOutput(format, view, uav, reason)) return false;

    // ── Samplers ──
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_Device->CreateSamplerState(&sampler, m_Linear.ReleaseAndGetAddressOf()))) {
        reason = "could not create the linear sampler";
        return false;
    }
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(m_Device->CreateSamplerState(&sampler, m_Point.ReleaseAndGetAddressOf()))) {
        reason = "could not create the point sampler";
        return false;
    }

    // ── Shaders and constants ──
    const std::wstring path = shaderDir + L"\\" + std::wstring(v.file.begin(), v.file.end());
    std::vector<std::pair<std::string, std::string>> defines;
    defines.push_back({"MW_FP16", v.fp16 ? "1" : "0"});

    if (v.file == "filters.hlsl" || v.file == "sgsr1.hlsl") {
        const double scaleX = static_cast<double>(m_Src.w) / m_Dst.w;
        const double scaleY = static_cast<double>(m_Src.h) / m_Dst.h;
        const double dilate = v.dilated ? std::max(1.0, std::max(scaleX, scaleY)) : 1.0;
        const int taps = static_cast<int>(std::ceil(2.0 * v.radius * dilate)) + 1;
        auto num = [](double d) {
            std::ostringstream s;
            s.precision(9);
            s << std::fixed << d;
            return s.str();
        };
        defines.push_back({"MW_B", num(v.cubicB)});
        defines.push_back({"MW_C", num(v.cubicC)});
        defines.push_back({"MW_KERNEL", std::to_string(v.kernel)});
        defines.push_back({"MW_RADIUS", std::to_string(v.radius)});
        defines.push_back({"MW_DILATE", num(dilate)});
        defines.push_back({"MW_TAPS", std::to_string(taps)});
        if (v.dilated) {
            std::ostringstream n;
            n << taps << "x" << taps << " taps";
            m_Notes = n.str();
        }

        ComPtr<ID3DBlob> vs;
        ComPtr<ID3DBlob> ps;
        if (!compile(path, "VsMain", "vs_5_0", defines, vs, reason)) return false;
        if (!compile(path, v.entry.c_str(), "ps_5_0", defines, ps, reason)) return false;
        if (FAILED(m_Device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                                                nullptr, m_Vs.ReleaseAndGetAddressOf())) ||
            FAILED(m_Device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr,
                                               m_Ps.ReleaseAndGetAddressOf()))) {
            reason = "could not create the shaders";
            return false;
        }

        Params p = {};
        p.srcSize[0] = static_cast<float>(m_Src.w);
        p.srcSize[1] = static_cast<float>(m_Src.h);
        p.invSrcSize[0] = 1.0f / m_Src.w;
        p.invSrcSize[1] = 1.0f / m_Src.h;
        p.dstSize[0] = static_cast<float>(m_Dst.w);
        p.dstSize[1] = static_cast<float>(m_Dst.h);
        p.invDstSize[0] = 1.0f / m_Dst.w;
        p.invDstSize[1] = 1.0f / m_Dst.h;
        p.scale[0] = static_cast<float>(scaleX);
        p.scale[1] = static_cast<float>(scaleY);
        p.invScale[0] = static_cast<float>(1.0 / scaleX);
        p.invScale[1] = static_cast<float>(1.0 / scaleY);
        return createConstants(m_Device.Get(), &p, sizeof(p), m_Constants, reason);
    }

    if (v.file == "fsr1.hlsl") {
        defines.push_back({"MW_RCAS", "0"});
        ComPtr<ID3DBlob> cs;
        if (!compile(path, v.entry.c_str(), "cs_5_0", defines, cs, reason)) return false;
        if (FAILED(m_Device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(),
                                                 nullptr, m_Cs.ReleaseAndGetAddressOf()))) {
            reason = "could not create the EASU shader";
            return false;
        }
        varAU4(con0);
        varAU4(con1);
        varAU4(con2);
        varAU4(con3);
        FsrEasuCon(con0, con1, con2, con3, static_cast<AF1>(m_Src.w), static_cast<AF1>(m_Src.h),
                   static_cast<AF1>(m_Src.w), static_cast<AF1>(m_Src.h), static_cast<AF1>(m_Dst.w),
                   static_cast<AF1>(m_Dst.h));
        AU1 consts[16];
        std::memcpy(consts, con0, 16);
        std::memcpy(consts + 4, con1, 16);
        std::memcpy(consts + 8, con2, 16);
        std::memcpy(consts + 12, con3, 16);
        if (!createConstants(m_Device.Get(), consts, sizeof(consts), m_Constants, reason))
            return false;
        m_GroupsX = (m_Dst.w + 15) / 16;
        m_GroupsY = (m_Dst.h + 15) / 16;

        if (v.rcas) {
            defines.back() = {"MW_RCAS", "1"};
            ComPtr<ID3DBlob> rcas;
            if (!compile(path, v.entry.c_str(), "cs_5_0", defines, rcas, reason)) return false;
            if (FAILED(m_Device->CreateComputeShader(rcas->GetBufferPointer(),
                                                     rcas->GetBufferSize(), nullptr,
                                                     m_CsRcas.ReleaseAndGetAddressOf()))) {
                reason = "could not create the RCAS shader";
                return false;
            }
            varAU4(rc);
            FsrRcasCon(rc, 0.2f);
            AU1 rconsts[16] = {};
            std::memcpy(rconsts, rc, 16);
            if (!createConstants(m_Device.Get(), rconsts, sizeof(rconsts), m_ConstantsRcas, reason))
                return false;

            // EASU writes here, RCAS reads it.
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = m_Dst.w;
            desc.Height = m_Dst.h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format =
                format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, m_Mid.ReleaseAndGetAddressOf())) ||
                FAILED(m_Device->CreateShaderResourceView(m_Mid.Get(), nullptr,
                                                          m_MidSrv.ReleaseAndGetAddressOf())) ||
                FAILED(m_Device->CreateUnorderedAccessView(m_Mid.Get(), nullptr,
                                                           m_MidUav.ReleaseAndGetAddressOf()))) {
                reason = "could not create the EASU→RCAS intermediate";
                return false;
            }
        }
        return true;
    }

    if (v.file == "nis.hlsl") {
        // The SDK's own contract first: NVScalerUpdateConfig refuses a scale
        // outside 1x..2x, and that is the answer for every case here.
        NISConfig config = {};
        const bool accepted = NVScalerUpdateConfig(
            config, 0.5f, 0, 0, m_Src.w, m_Src.h, m_Src.w, m_Src.h, 0, 0, m_Dst.w, m_Dst.h, m_Dst.w,
            m_Dst.h, range == Range::Hdr ? NISHDRMode::PQ : NISHDRMode::None);
        if (!accepted) {
            m_Unsupported = true;
            std::ostringstream why;
            why << "NVScalerUpdateConfig refuses scale " << std::fixed
                << static_cast<double>(m_Src.w) / m_Dst.w
                << " (NIS accepts 0.5..1, i.e. upscaling only)";
            reason = why.str();
            return false;
        }
        if (range == Range::Hdr) defines.push_back({"MW_NIS_HDR", "2"});
        ComPtr<ID3DBlob> cs;
        if (!compile(path, v.entry.c_str(), "cs_5_0", defines, cs, reason)) return false;
        if (FAILED(m_Device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(),
                                                 nullptr, m_Cs.ReleaseAndGetAddressOf()))) {
            reason = "could not create the NIS shader";
            return false;
        }
        if (!createConstants(m_Device.Get(), &config, sizeof(config), m_Constants, reason))
            return false;

        // The two coefficient tables, as float4 textures of kFilterSize/4 × kPhaseCount.
        auto coefTexture = [&](const float(*table)[kFilterSize],
                               ComPtr<ID3D11ShaderResourceView>& out) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = kFilterSize / 4;
            desc.Height = kPhaseCount;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init = {};
            init.pSysMem = table;
            init.SysMemPitch = kFilterSize * sizeof(float);
            ComPtr<ID3D11Texture2D> tex;
            return SUCCEEDED(m_Device->CreateTexture2D(&desc, &init, tex.GetAddressOf())) &&
                   SUCCEEDED(m_Device->CreateShaderResourceView(tex.Get(), nullptr,
                                                                out.ReleaseAndGetAddressOf()));
        };
        if (!coefTexture(coef_scale, m_NisCoefScale) || !coefTexture(coef_usm, m_NisCoefUsm)) {
            reason = "could not upload the NIS coefficients";
            return false;
        }
        m_GroupsX = (m_Dst.w + 31) / 32;
        m_GroupsY = (m_Dst.h + 23) / 24;
        return true;
    }

    reason = "unknown shader file " + v.file;
    return false;
}

bool Pass::initVideoProcessor(const Gpu& gpu, const Variant& v, const Case& c, Range range,
                              SourceSet& sources, std::string& reason)
{
    (void)c;
    if (FAILED(gpu.device.As(&m_VideoDevice)) || FAILED(gpu.context.As(&m_VideoContext))) {
        m_Unsupported = true;
        reason = "the device has no video processing interface";
        return false;
    }

    const DXGI_FORMAT inputFormat =
        range == Range::Sdr ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    const DXGI_FORMAT outputFormat = inputFormat;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = m_Src.w;
    content.InputHeight = m_Src.h;
    content.OutputWidth = m_Dst.w;
    content.OutputHeight = m_Dst.h;
    content.Usage = static_cast<D3D11_VIDEO_USAGE>(v.vpUsage);
    HRESULT hr =
        m_VideoDevice->CreateVideoProcessorEnumerator(&content, m_VpEnum.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        m_Unsupported = true;
        reason = "no video processor for this content (" + hex(hr) + ")";
        return false;
    }
    UINT flags = 0;
    if (FAILED(m_VpEnum->CheckVideoProcessorFormat(inputFormat, &flags)) ||
        !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        m_Unsupported = true;
        reason = "the video processor does not take this input format";
        return false;
    }
    if (FAILED(m_VpEnum->CheckVideoProcessorFormat(outputFormat, &flags)) ||
        !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        m_Unsupported = true;
        reason = "the video processor does not write this output format";
        return false;
    }
    hr = m_VideoDevice->CreateVideoProcessor(m_VpEnum.Get(), 0, m_Vp.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        reason = "could not create the video processor (" + hex(hr) + ")";
        return false;
    }

    // Inputs: the typed 8-bit picture, or the FP16 one.
    const SourceSet::Flavour flavour =
        range == Range::Sdr ? SourceSet::Flavour::Rgba8 : SourceSet::Flavour::Half16;
    for (int i = 0; i < 2; ++i) {
        m_Input[i] = sources.get(flavour, i == 1, false, reason);
        if (!m_Input[i]) return false;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in = {};
        in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        hr = m_VideoDevice->CreateVideoProcessorInputView(
            m_Input[i]->texture.Get(), m_VpEnum.Get(), &in, m_VpInput[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            reason = "could not create the video processor's input view (" + hex(hr) + ")";
            return false;
        }
    }

    if (!createOutput(outputFormat, outputFormat, false, reason)) return false;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out = {};
    out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = m_VideoDevice->CreateVideoProcessorOutputView(m_Output.Get(), m_VpEnum.Get(), &out,
                                                       m_VpOutput.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        reason = "could not create the video processor's output view (" + hex(hr) + ")";
        return false;
    }

    // Stream state: the whole source to the whole output, progressive, and
    // the colour spaces named so the driver has no reason to guess.
    const RECT srcRect = {0, 0, m_Src.w, m_Src.h};
    const RECT dstRect = {0, 0, m_Dst.w, m_Dst.h};
    m_VideoContext->VideoProcessorSetStreamFrameFormat(m_Vp.Get(), 0,
                                                       D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_VideoContext->VideoProcessorSetStreamSourceRect(m_Vp.Get(), 0, TRUE, &srcRect);
    m_VideoContext->VideoProcessorSetStreamDestRect(m_Vp.Get(), 0, TRUE, &dstRect);
    m_VideoContext->VideoProcessorSetOutputTargetRect(m_Vp.Get(), TRUE, &dstRect);
    m_VideoContext->VideoProcessorSetStreamAutoProcessingMode(m_Vp.Get(), 0,
                                                              v.vpAuto ? TRUE : FALSE);

    ComPtr<ID3D11VideoContext1> context1;
    if (SUCCEEDED(m_VideoContext.As(&context1))) {
        const DXGI_COLOR_SPACE_TYPE space = range == Range::Sdr
                                                ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                                                : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        context1->VideoProcessorSetStreamColorSpace1(m_Vp.Get(), 0, space);
        context1->VideoProcessorSetOutputColorSpace1(m_Vp.Get(), space);
        m_Notes = range == Range::Sdr ? "colour space RGB_FULL_G22_NONE_P709 in and out"
                                      : "colour space RGB_FULL_G10_NONE_P709 (scRGB) in and out";
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE legacy = {};
        legacy.RGB_Range = 0;
        legacy.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        m_VideoContext->VideoProcessorSetStreamColorSpace(m_Vp.Get(), 0, &legacy);
        m_VideoContext->VideoProcessorSetOutputColorSpace(m_Vp.Get(), &legacy);
        m_Notes = "legacy colour space API (no ID3D11VideoContext1)";
    }
    return true;
}

void Pass::run(bool shifted)
{
    const int i = shifted ? 1 : 0;
    switch (m_Variant.kind) {
    case Kind::VideoProcessor: {
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = m_VpInput[i].Get();
        m_VideoContext->VideoProcessorBlt(m_Vp.Get(), m_VpOutput.Get(), 0, 1, &stream);
        return;
    }
    case Kind::Compute: {
        ID3D11ShaderResourceView* srvs[3] = {m_InputView[i].Get(), m_NisCoefScale.Get(),
                                             m_NisCoefUsm.Get()};
        ID3D11SamplerState* samplers[1] = {m_Linear.Get()};
        ID3D11Buffer* cbs[1] = {m_Constants.Get()};
        ID3D11UnorderedAccessView* uav = m_CsRcas ? m_MidUav.Get() : m_OutputUav.Get();
        m_Context->CSSetShader(m_Cs.Get(), nullptr, 0);
        m_Context->CSSetShaderResources(0, 3, srvs);
        m_Context->CSSetSamplers(0, 1, samplers);
        m_Context->CSSetConstantBuffers(0, 1, cbs);
        m_Context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        m_Context->Dispatch(m_GroupsX, m_GroupsY, 1);
        if (m_CsRcas) {
            ID3D11UnorderedAccessView* none = nullptr;
            ID3D11ShaderResourceView* mid[1] = {m_MidSrv.Get()};
            ID3D11Buffer* rcb[1] = {m_ConstantsRcas.Get()};
            ID3D11UnorderedAccessView* out = m_OutputUav.Get();
            m_Context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
            m_Context->CSSetShader(m_CsRcas.Get(), nullptr, 0);
            m_Context->CSSetShaderResources(0, 1, mid);
            m_Context->CSSetConstantBuffers(0, 1, rcb);
            m_Context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
            m_Context->Dispatch(m_GroupsX, m_GroupsY, 1);
        }
        ID3D11UnorderedAccessView* none = nullptr;
        ID3D11ShaderResourceView* nosrv[3] = {nullptr, nullptr, nullptr};
        m_Context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        m_Context->CSSetShaderResources(0, 3, nosrv);
        return;
    }
    case Kind::Mip: m_Context->GenerateMips(m_InputView[i].Get()); [[fallthrough]];
    case Kind::Pixel: {
        ID3D11RenderTargetView* rtv = m_OutputRtv.Get();
        ID3D11ShaderResourceView* srvs[1] = {m_InputView[i].Get()};
        ID3D11SamplerState* samplers[2] = {m_Linear.Get(), m_Point.Get()};
        ID3D11Buffer* cbs[1] = {m_Constants.Get()};
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(m_Dst.w);
        viewport.Height = static_cast<float>(m_Dst.h);
        viewport.MaxDepth = 1.0f;
        m_Context->OMSetRenderTargets(1, &rtv, nullptr);
        m_Context->RSSetViewports(1, &viewport);
        m_Context->IASetInputLayout(nullptr);
        m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_Context->VSSetShader(m_Vs.Get(), nullptr, 0);
        m_Context->PSSetShader(m_Ps.Get(), nullptr, 0);
        m_Context->PSSetShaderResources(0, 1, srvs);
        m_Context->PSSetSamplers(0, 2, samplers);
        m_Context->PSSetConstantBuffers(0, 1, cbs);
        m_Context->Draw(3, 0);
        ID3D11RenderTargetView* none = nullptr;
        ID3D11ShaderResourceView* nosrv[1] = {nullptr};
        m_Context->OMSetRenderTargets(1, &none, nullptr);
        m_Context->PSSetShaderResources(0, 1, nosrv);
        return;
    }
    }
}

bool Pass::readback(ImageF& encoded, std::string& error)
{
    m_Context->CopyResource(m_Staging.Get(), m_Output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_Context->Map(m_Staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "could not map the output";
        return false;
    }
    encoded = ImageF(m_Dst.w, m_Dst.h);
    const auto* base = static_cast<const uint8_t*>(mapped.pData);
    switch (m_OutputFormat) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        for (int y = 0; y < m_Dst.h; ++y) {
            const uint8_t* row = base + static_cast<size_t>(y) * mapped.RowPitch;
            for (int x = 0; x < m_Dst.w; ++x) {
                float* o = encoded.at(x, y);
                for (int c = 0; c < 3; ++c)
                    o[c] = row[x * 4 + c] / 255.0f;
            }
        }
        break;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        for (int y = 0; y < m_Dst.h; ++y) {
            const auto* row =
                reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * mapped.RowPitch);
            for (int x = 0; x < m_Dst.w; ++x) {
                float* o = encoded.at(x, y);
                for (int c = 0; c < 3; ++c)
                    o[c] = row[x * 4 + c] / 65535.0f;
            }
        }
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        // Linear scRGB out: encode to PQ so it is scored where the others are.
        ImageF linear(m_Dst.w, m_Dst.h);
        for (int y = 0; y < m_Dst.h; ++y) {
            const auto* row =
                reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * mapped.RowPitch);
            for (int x = 0; x < m_Dst.w; ++x) {
                float* o = linear.at(x, y);
                for (int c = 0; c < 3; ++c)
                    o[c] = image::halfToFloat(row[x * 4 + c]);
            }
        }
        encoded = image::pqEncode(linear);
        break;
    }
    default:
        m_Context->Unmap(m_Staging.Get(), 0);
        error = "unexpected output format";
        return false;
    }
    m_Context->Unmap(m_Staging.Get(), 0);
    return true;
}

} // namespace bench
