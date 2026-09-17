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

#pragma once

#include "Adapters.h"
#include "Bench.h"
#include "Scalers.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <map>
#include <string>

namespace bench {

/// The inputs of one (case, range) as GPU textures, each flavour made the
/// first time a candidate asks for it and kept for the rest of the run.
///
/// SDR keeps the 8-bit sRGB picture as a TYPELESS texture so the same bytes
/// can be read through a UNORM view (perceptual filtering, what the host does
/// today) or a UNORM_SRGB one (the sampler decodes to linear light for free).
/// HDR keeps the scRGB picture as FP16 for linear candidates and its PQ
/// encoding as UNORM16 for perceptual ones — encoded on the CPU by the same
/// code as the reference, so nothing GPU-side is in the comparison.
class SourceSet
{
public:
    enum class Flavour
    {
        /// R8G8B8A8_TYPELESS, the SDR picture.
        Srgb8,
        /// R8G8B8A8_UNORM typed, for the video processor's input view.
        Rgba8,
        /// R16G16B16A16_FLOAT, scRGB.
        Half16,
        /// R16G16B16A16_UNORM, PQ codes.
        Pq16
    };

    struct Tex
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvSrgb;
    };

    /// @p encoded is sRGB values (SDR) or PQ codes (HDR); @p linear is the
    /// scRGB picture, HDR only. The shifted twins feed the phase test. The
    /// pictures must outlive the set.
    void init(ID3D11Device* device, Range range, const ImageF* encoded,
              const ImageF* encodedShifted, const ImageF* linear, const ImageF* linearShifted);

    const Tex* get(Flavour flavour, bool shifted, bool mips, std::string& error);

    int width() const { return m_Encoded ? m_Encoded->w : 0; }
    int height() const { return m_Encoded ? m_Encoded->h : 0; }

private:
    ID3D11Device* m_Device = nullptr;
    Range m_Range = Range::Sdr;
    const ImageF* m_Encoded = nullptr;
    const ImageF* m_EncodedShifted = nullptr;
    const ImageF* m_Linear = nullptr;
    const ImageF* m_LinearShifted = nullptr;
    std::map<int, Tex> m_Cache;
};

/// One candidate, ready to run on one GPU for one case: shaders compiled,
/// output allocated, views built. run() is the thing the timer wraps.
class Pass
{
public:
    /// Returns false when the candidate cannot run here; `unsupported()`
    /// then says whether that is by contract (reported as such) or a fault.
    bool init(const Gpu& gpu, const Variant& variant, const Case& c, Range range,
              SourceSet& sources, const std::wstring& shaderDir, std::string& reason);

    bool unsupported() const { return m_Unsupported; }

    /// Whatever the candidate does, once, on the normal or the shifted input.
    void run(bool shifted);

    /// The last output, in the encoded domain (sRGB values or PQ codes).
    bool readback(ImageF& encoded, std::string& error);

    /// What the pass wants the report to know (e.g. the colour space the
    /// driver accepted).
    const std::string& notes() const { return m_Notes; }

private:
    bool initShader(const Gpu& gpu, const Variant& v, const Case& c, Range range,
                    SourceSet& sources, const std::wstring& shaderDir, std::string& reason);
    bool initVideoProcessor(const Gpu& gpu, const Variant& v, const Case& c, Range range,
                            SourceSet& sources, std::string& reason);
    bool createOutput(DXGI_FORMAT format, DXGI_FORMAT viewFormat, bool uav, std::string& reason);
    bool compile(const std::wstring& path, const char* entry, const char* target,
                 const std::vector<std::pair<std::string, std::string>>& defines,
                 Microsoft::WRL::ComPtr<ID3DBlob>& blob, std::string& reason);

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
    Variant m_Variant;
    Range m_Range = Range::Sdr;
    Size m_Src;
    Size m_Dst;
    bool m_Unsupported = false;
    std::string m_Notes;

    // Inputs, normal and shifted.
    const SourceSet::Tex* m_Input[2] = {nullptr, nullptr};
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_InputView[2];

    // Output.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Output;
    DXGI_FORMAT m_OutputFormat = DXGI_FORMAT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_OutputRtv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_OutputUav;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Staging;

    // Shader path.
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_Vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_Ps;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_Cs;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_CsRcas;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_Linear;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_Point;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_Constants;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ConstantsRcas;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Mid;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_MidSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_MidUav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_NisCoefScale;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_NisCoefUsm;
    UINT m_GroupsX = 0;
    UINT m_GroupsY = 0;

    // Video processor path.
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_VideoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_VideoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_VpEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_Vp;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_VpInput[2];
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_VpOutput;
};

} // namespace bench
