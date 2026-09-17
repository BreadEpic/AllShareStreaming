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

#include "Image.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace bench::image {

namespace {

bool factory(ComPtr<IWICImagingFactory>& out, std::string& error)
{
    const HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(out.GetAddressOf()));
    if (FAILED(hr)) {
        error = "WIC is unavailable (CoInitialize missing?)";
        return false;
    }
    return true;
}

} // namespace

bool loadPng(const std::wstring& path, ImageRgba8& out, std::string& error)
{
    ComPtr<IWICImagingFactory> wic;
    if (!factory(wic, error)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand,
                                              decoder.GetAddressOf()))) {
        error = "could not open the picture";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
        error = "could not decode the picture";
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        error = "could not convert the picture to RGBA8";
        return false;
    }
    UINT w = 0;
    UINT h = 0;
    converter->GetSize(&w, &h);
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.px.resize(static_cast<size_t>(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.px.size()),
                                     out.px.data()))) {
        error = "could not read the picture's pixels";
        return false;
    }
    return true;
}

bool savePng(const std::wstring& path, const ImageRgba8& img, std::string& error)
{
    ComPtr<IWICImagingFactory> wic;
    if (!factory(wic, error)) return false;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) ||
        FAILED(frame->Initialize(nullptr))) {
        error = "could not create the PNG";
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetSize(img.w, img.h)) || FAILED(frame->SetPixelFormat(&format))) {
        error = "could not set the PNG's format";
        return false;
    }
    std::vector<uint8_t> pixels = img.px;
    if (format != GUID_WICPixelFormat32bppRGBA) {
        // The encoder prefers BGRA; swap in place.
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            std::swap(pixels[i], pixels[i + 2]);
    }
    if (FAILED(frame->WritePixels(img.h, img.w * 4, static_cast<UINT>(pixels.size()),
                                  pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        error = "could not write the PNG";
        return false;
    }
    return true;
}

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float c)
{
    c = std::max(c, 0.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

ImageF decodeSrgb(const ImageRgba8& img)
{
    // A 256-entry table: the picture is millions of pixels, pow() is not free.
    float table[256];
    for (int i = 0; i < 256; ++i)
        table[i] = srgbToLinear(i / 255.0f);
    ImageF out(img.w, img.h);
    const size_t n = static_cast<size_t>(img.w) * img.h;
    for (size_t i = 0; i < n; ++i) {
        out.px[i * 3 + 0] = table[img.px[i * 4 + 0]];
        out.px[i * 3 + 1] = table[img.px[i * 4 + 1]];
        out.px[i * 3 + 2] = table[img.px[i * 4 + 2]];
    }
    return out;
}

ImageF srgbEncode(const ImageF& linear)
{
    ImageF out(linear.w, linear.h);
    for (size_t i = 0; i < out.px.size(); ++i)
        out.px[i] = linearToSrgb(linear.px[i]);
    return out;
}

ImageF srgbDecode(const ImageF& encoded)
{
    ImageF out(encoded.w, encoded.h);
    for (size_t i = 0; i < out.px.size(); ++i)
        out.px[i] = srgbToLinear(encoded.px[i]);
    return out;
}

ImageF pqEncode(const ImageF& scRgb)
{
    // The host's constants (ColorConvert.cpp), to the digit.
    static const float m[9] = {0.6274040f, 0.3292820f, 0.0433136f, 0.0690970f, 0.9195400f,
                               0.0113612f, 0.0163916f, 0.0880132f, 0.8956050f};
    constexpr float kM1 = 0.1593017578125f;
    constexpr float kM2 = 78.84375f;
    constexpr float kC1 = 0.8359375f;
    constexpr float kC2 = 18.8515625f;
    constexpr float kC3 = 18.6875f;
    constexpr float kScRgbToPq = 80.0f / 10000.0f;

    ImageF out(scRgb.w, scRgb.h);
    const size_t n = static_cast<size_t>(scRgb.w) * scRgb.h;
    for (size_t i = 0; i < n; ++i) {
        const float* in = &scRgb.px[i * 3];
        float* o = &out.px[i * 3];
        for (int c = 0; c < 3; ++c) {
            const float v = m[c * 3] * in[0] + m[c * 3 + 1] * in[1] + m[c * 3 + 2] * in[2];
            const float y = std::max(v, 0.0f) * kScRgbToPq;
            const float ym = std::pow(y, kM1);
            o[c] = std::pow((kC1 + kC2 * ym) / (1.0f + kC3 * ym), kM2);
        }
    }
    return out;
}

ImageF synthesizeHdr(const ImageF& linear, std::string& description)
{
    ImageF out = linear;
    const size_t n = static_cast<size_t>(out.w) * out.h;

    // The 98th percentile of luma, by histogram — deterministic and cheap.
    std::vector<uint32_t> hist(1024, 0);
    for (size_t i = 0; i < n; ++i) {
        const float* p = out.at(0, 0) + i * 3;
        const float y = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        hist[std::min(1023, static_cast<int>(y * 1023.0f))]++;
    }
    size_t seen = 0;
    int cut = 1023;
    for (int b = 0; b < 1024; ++b) {
        seen += hist[b];
        if (seen >= n * 98 / 100) {
            cut = b;
            break;
        }
    }
    const float threshold = cut / 1023.0f;
    constexpr float kPeak = 10.0f; // 800 nits in scRGB
    for (size_t i = 0; i < n; ++i) {
        float* p = out.at(0, 0) + i * 3;
        const float y = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        if (y > threshold && threshold < 1.0f) {
            const float t = (y - threshold) / (1.0f - threshold);
            const float gain = 1.0f + t * t * (kPeak - 1.0f);
            p[0] *= gain;
            p[1] *= gain;
            p[2] *= gain;
        }
    }

    // Four patches along the bottom edge, each a 1/16th of the width, one
    // 1/24th of the height tall.
    const int pw = out.w / 16;
    const int ph = out.h / 24;
    const int py = out.h - ph - ph / 2;
    const float levels[3] = {4.0f, 8.0f, 12.0f};
    for (int k = 0; k < 4; ++k) {
        const int px = out.w / 8 + k * (out.w / 5);
        for (int y = py; y < py + ph && y < out.h; ++y) {
            for (int x = px; x < px + pw && x < out.w; ++x) {
                float* p = out.at(x, y);
                float v = 0.0f;
                if (k < 3) {
                    v = levels[k];
                } else {
                    // A 1-pixel grid at 8.0 on black, 6 pixels apart.
                    v = ((x - px) % 6 == 0 || (y - py) % 6 == 0) ? 8.0f : 0.0f;
                }
                p[0] = p[1] = p[2] = v;
            }
        }
    }

    std::ostringstream d;
    d << "sRGB decoded to linear, SDR white = 1.0 (80 nits); luma above the 98th percentile ("
      << threshold << ") pushed on a quadratic ramp to " << kPeak
      << " (800 nits); four synthetic patches along the bottom edge at 4.0, 8.0, 12.0 and a "
         "1-pixel grid at 8.0 on black.";
    description = d.str();
    return out;
}

ImageF shifted(const ImageF& in, int dx, int dy)
{
    ImageF out(in.w, in.h);
    for (int y = 0; y < in.h; ++y) {
        const int sy = std::clamp(y - dy, 0, in.h - 1);
        for (int x = 0; x < in.w; ++x) {
            const int sx = std::clamp(x - dx, 0, in.w - 1);
            std::memcpy(out.at(x, y), in.at(sx, sy), 3 * sizeof(float));
        }
    }
    return out;
}

uint16_t floatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t half = mant >> shift;
        // Round to nearest even.
        const uint32_t rem = mant & ((1u << shift) - 1);
        const uint32_t midpoint = 1u << (shift - 1);
        if (rem > midpoint || (rem == midpoint && (half & 1))) half++;
        return static_cast<uint16_t>(sign | half);
    }
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    uint32_t half = static_cast<uint32_t>(exp << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1))) half++;
    return static_cast<uint16_t>(sign | half);
}

float halfToFloat(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3FFu;
            out = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &out, 4);
    return f;
}

std::vector<uint8_t> toRgba8(const ImageF& encoded)
{
    const size_t n = static_cast<size_t>(encoded.w) * encoded.h;
    std::vector<uint8_t> out(n * 4);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = std::clamp(encoded.px[i * 3 + c], 0.0f, 1.0f);
            out[i * 4 + c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
        out[i * 4 + 3] = 255;
    }
    return out;
}

std::vector<uint16_t> toRgba16Unorm(const ImageF& encoded)
{
    const size_t n = static_cast<size_t>(encoded.w) * encoded.h;
    std::vector<uint16_t> out(n * 4);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = std::clamp(encoded.px[i * 3 + c], 0.0f, 1.0f);
            out[i * 4 + c] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
        }
        out[i * 4 + 3] = 65535;
    }
    return out;
}

std::vector<uint16_t> toRgba16Half(const ImageF& linear)
{
    const size_t n = static_cast<size_t>(linear.w) * linear.h;
    std::vector<uint16_t> out(n * 4);
    const uint16_t one = floatToHalf(1.0f);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c)
            out[i * 4 + c] = floatToHalf(linear.px[i * 3 + c]);
        out[i * 4 + 3] = one;
    }
    return out;
}

ImageRgba8 crop8(const ImageF& encoded, int x0, int y0, int w, int h)
{
    ImageRgba8 out;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int sx = std::clamp(x0 + x, 0, encoded.w - 1);
            const int sy = std::clamp(y0 + y, 0, encoded.h - 1);
            const float* p = encoded.at(sx, sy);
            uint8_t* o = &out.px[(static_cast<size_t>(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c)
                o[c] = static_cast<uint8_t>(std::clamp(p[c], 0.0f, 1.0f) * 255.0f + 0.5f);
            o[3] = 255;
        }
    }
    return out;
}

} // namespace bench::image
