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

#include "PictureConverter.h"

#include "../../core/Log.h"

namespace mw::native::convert {

bool PictureConverter::init(bool compute, ID3D11Device* device, DXGI_FORMAT sourceFormat,
                            int sourceWidth, int sourceHeight, int outputWidth, int outputHeight,
                            Chroma chroma, bool hdr, ScaleFilter filter, std::string& error)
{
    m_Compute.reset();
    m_Context.reset();
    m_D3d11.reset();
    m_Device = device;
    m_SourceFormat = sourceFormat;
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    m_OutputWidth = outputWidth;
    m_OutputHeight = outputHeight;
    m_Chroma = chroma;
    m_Hdr = hdr;
    m_Filter = filter;

    if (compute) {
        std::string reason;
        if (chroma == Chroma::C444) {
            reason = "4:4:4 has no D3D12 path";
        } else {
            auto context = std::make_unique<platform::D3d12Context>();
            auto converter = std::make_unique<ComputeConvert>();
            if (context->init(device, reason) &&
                converter->init(*context, sourceFormat, sourceWidth, sourceHeight, outputWidth,
                                outputHeight, hdr, filter, device, reason)) {
                m_Context = std::move(context);
                m_Compute = std::move(converter);
                m_Compute->setSdrWhite(m_SdrWhite);
                return true;
            }
        }
        log::info("[native] no D3D12 conversion for this session (" + reason +
                  ") — converting on the D3D11 device");
    }
    return initD3d11(error);
}

bool PictureConverter::initD3d11(std::string& error)
{
    m_D3d11 = std::make_unique<ColorConvert>();
    if (!m_D3d11->init(m_Device.Get(), m_SourceFormat, m_SourceWidth, m_SourceHeight, m_OutputWidth,
                       m_OutputHeight, m_Chroma, m_Hdr, m_Filter, error))
        return false;
    m_D3d11->setSdrWhite(m_SdrWhite);
    return true;
}

void PictureConverter::fallBack(const std::string& reason)
{
    log::warning("[native] the D3D12 conversion gave up (" + reason +
                 ") — back on the D3D11 device for the rest of the session");
    // The converter before its context: it holds resources of that device.
    m_Compute.reset();
    m_Context.reset();
}

UINT PictureConverter::sessionTextureMiscFlags() const
{
    return m_Compute ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED : 0u;
}

void PictureConverter::wroteOnD3d11(ID3D11DeviceContext* context)
{
    if (m_Compute) m_Context->after(context);
}

bool PictureConverter::convert(ID3D11Texture2D* source, const capture::CursorState& cursor,
                               const CursorDraw& draw, std::string& error)
{
    if (m_Compute) {
        std::string reason;
        ID3D12Resource* opened = m_Context->open(source, reason);
        if (opened && m_Compute->convert(opened, cursor, draw, reason)) return true;
        fallBack(reason);
        if (!initD3d11(error)) return false;
    }
    if (!m_D3d11) {
        error = "colour conversion is not initialized";
        return false;
    }
    return m_D3d11->convert(source, cursor, draw, error);
}

ID3D11Texture2D* PictureConverter::output() const
{
    if (m_Compute) return m_Compute->outputD3d11();
    return m_D3d11 ? m_D3d11->output() : nullptr;
}

int PictureConverter::outputWidth() const
{
    if (m_Compute) return m_Compute->outputWidth();
    return m_D3d11 ? m_D3d11->outputWidth() : 0;
}

int PictureConverter::outputHeight() const
{
    if (m_Compute) return m_Compute->outputHeight();
    return m_D3d11 ? m_D3d11->outputHeight() : 0;
}

PictureConverter::ScaleFilter PictureConverter::scaleFilter() const
{
    if (m_Compute) return m_Compute->scaleFilter();
    return m_D3d11 ? m_D3d11->scaleFilter() : ScaleFilter::Bilinear;
}

bool PictureConverter::dropResample()
{
    if (m_Compute) return m_Compute->dropResample();
    return m_D3d11 && m_D3d11->dropResample();
}

bool PictureConverter::letterboxed() const
{
    if (m_Compute) return m_Compute->letterboxed();
    return m_D3d11 && m_D3d11->letterboxed();
}

bool PictureConverter::toneMapsToSdr() const
{
    if (m_Compute) return m_Compute->toneMapsToSdr();
    return m_D3d11 && m_D3d11->toneMapsToSdr();
}

bool PictureConverter::scRgbSource() const
{
    if (m_Compute) return m_Compute->scRgbSource();
    return m_D3d11 && m_D3d11->scRgbSource();
}

void PictureConverter::setSdrWhite(float scRgbWhite)
{
    m_SdrWhite = scRgbWhite;
    if (m_Compute) m_Compute->setSdrWhite(scRgbWhite);
    if (m_D3d11) m_D3d11->setSdrWhite(scRgbWhite);
}

} // namespace mw::native::convert
