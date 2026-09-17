/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "convert/windows/ColorConvert.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace mw::native;
#endif

// The resample path of ColorConvert, on WARP so it runs on a headless runner:
// a picture nothing but a 1-pixel checkerboard, halved. A real low-pass in
// linear light gives the grey of half-light (sRGB 0.735, luma code 177); the
// bilinear pass, averaging in gamma space, gives sRGB 0.5 (code 126). The two
// codes are far enough apart that the test cannot mistake one path for the
// other — which is the point: the filter must be the one asked for, and it
// must be in light.
void run_color_convert_tests()
{
#if defined(_WIN32)
    using convert::ColorConvert;
    using Microsoft::WRL::ComPtr;
    using ScaleFilter = ColorConvert::ScaleFilter;

    SECTION("ColorConvert: the resample path on WARP");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, wanted, 2, D3D11_SDK_VERSION,
                                   device.GetAddressOf(), nullptr, context.GetAddressOf()))) {
        std::fprintf(stderr, "  no WARP device here — skipped\n");
        return;
    }

    // A checkerboard of the given size, 8-bit sRGB, as the capture would hand
    // it over.
    auto checkerboard = [&](int w, int h) {
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const uint8_t v = ((x + y) & 1) ? 255 : 0;
                uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = p[1] = p[2] = v;
                p[3] = 255;
            }
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA seed = {};
        seed.pSysMem = px.data();
        seed.SysMemPitch = static_cast<UINT>(w * 4);
        ComPtr<ID3D11Texture2D> tex;
        device->CreateTexture2D(&desc, &seed, tex.GetAddressOf());
        return tex;
    };

    // The NV12 output, read back: luma rows first, the chroma plane after
    // them at the same pitch.
    struct Planes
    {
        std::vector<uint8_t> bytes;
        UINT pitch = 0;
        int w = 0;
        int h = 0;
        uint8_t luma(int x, int y) const { return bytes[static_cast<size_t>(y) * pitch + x]; }
        uint8_t chroma(int x, int y, int c) const
        {
            return bytes[static_cast<size_t>(h + y) * pitch + static_cast<size_t>(x) * 2 + c];
        }
    };
    auto readback = [&](ColorConvert& conv, Planes& out) {
        D3D11_TEXTURE2D_DESC desc = {};
        conv.output()->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return false;
        context->CopyResource(staging.Get(), conv.output());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        out.w = conv.outputWidth();
        out.h = conv.outputHeight();
        out.pitch = mapped.RowPitch;
        out.bytes.assign(static_cast<const uint8_t*>(mapped.pData),
                         static_cast<const uint8_t*>(mapped.pData) +
                             static_cast<size_t>(mapped.RowPitch) * out.h * 3 / 2);
        context->Unmap(staging.Get(), 0);
        return true;
    };
    auto within = [](int value, int expected, int tolerance) {
        return value >= expected - tolerance && value <= expected + tolerance;
    };

    const int kHalfLight = 177; // sRGB(0.5 linear) = 0.735 → 0.735 * 219 + 16
    const int kHalfGamma = 126; // 0.5 * 219 + 16
    const int kBlack = 16;
    const int kGrey = 128;

    // ── Halved through Lanczos-2: half-light grey, everywhere ──
    {
        ColorConvert conv;
        std::string error;
        ComPtr<ID3D11Texture2D> source = checkerboard(64, 64);
        CHECK(source != nullptr);
        CHECK(conv.init(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 64, 64, 32, 32,
                        ColorConvert::Chroma::C420, false, ScaleFilter::Lanczos2, error));
        if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
        CHECK(conv.scaleFilter() == ScaleFilter::Lanczos2);
        CHECK(!conv.letterboxed());
        CHECK(conv.convert(source.Get(), capture::CursorState{}, convert::CursorDraw{}, error));
        Planes p;
        CHECK(readback(conv, p));
        if (!p.bytes.empty()) {
            bool lumaOk = true;
            bool chromaOk = true;
            for (int y = 2; y < p.h - 2; ++y)
                for (int x = 2; x < p.w - 2; ++x)
                    lumaOk = lumaOk && within(p.luma(x, y), kHalfLight, 6);
            for (int y = 1; y < p.h / 2 - 1; ++y)
                for (int x = 1; x < p.w / 2 - 1; ++x)
                    chromaOk = chromaOk && within(p.chroma(x, y, 0), kGrey, 2) &&
                               within(p.chroma(x, y, 1), kGrey, 2);
            CHECK(lumaOk);
            CHECK(chromaOk);
            if (!lumaOk)
                std::fprintf(stderr, "  luma at (8,8) = %d, expected ~%d\n", p.luma(8, 8),
                             kHalfLight);
        }
    }

    // ── The same through the bilinear pass: the gamma-space grey ──
    {
        ColorConvert conv;
        std::string error;
        ComPtr<ID3D11Texture2D> source = checkerboard(64, 64);
        CHECK(conv.init(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 64, 64, 32, 32,
                        ColorConvert::Chroma::C420, false, ScaleFilter::Bilinear, error));
        CHECK(conv.scaleFilter() == ScaleFilter::Bilinear);
        CHECK(conv.convert(source.Get(), capture::CursorState{}, convert::CursorDraw{}, error));
        Planes p;
        CHECK(readback(conv, p));
        if (!p.bytes.empty()) {
            bool lumaOk = true;
            for (int y = 2; y < p.h - 2; ++y)
                for (int x = 2; x < p.w - 2; ++x)
                    lumaOk = lumaOk && within(p.luma(x, y), kHalfGamma, 6);
            CHECK(lumaOk);
            if (!lumaOk)
                std::fprintf(stderr, "  luma at (8,8) = %d, expected ~%d\n", p.luma(8, 8),
                             kHalfGamma);
        }
    }

    // ── 1:1 asks for nothing: the filter falls back to the plain pass ──
    {
        ColorConvert conv;
        std::string error;
        CHECK(conv.init(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 64, 64, 64, 64,
                        ColorConvert::Chroma::C420, false, ScaleFilter::Lanczos2, error));
        CHECK(conv.scaleFilter() == ScaleFilter::Bilinear);
        CHECK(!conv.letterboxed());
    }

    // ── Another shape: bars, black, and the picture centred between them ──
    {
        ColorConvert conv;
        std::string error;
        ComPtr<ID3D11Texture2D> source = checkerboard(64, 32);
        CHECK(conv.init(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 64, 32, 32, 32,
                        ColorConvert::Chroma::C420, false, ScaleFilter::Lanczos2, error));
        CHECK(conv.letterboxed());
        CHECK(conv.convert(source.Get(), capture::CursorState{}, convert::CursorDraw{}, error));
        Planes p;
        CHECK(readback(conv, p));
        if (!p.bytes.empty()) {
            // 64x32 into 32x32: a 32x16 picture, rows 8..23; bars above and below.
            bool barsOk = true;
            bool pictureOk = true;
            for (int x = 0; x < p.w; ++x) {
                for (int y = 0; y < 7; ++y)
                    barsOk = barsOk && p.luma(x, y) == kBlack;
                for (int y = 25; y < p.h; ++y)
                    barsOk = barsOk && p.luma(x, y) == kBlack;
                for (int y = 10; y < 22; ++y)
                    pictureOk = pictureOk && within(p.luma(x, y), kHalfLight, 6);
            }
            CHECK(barsOk);
            CHECK(pictureOk);
        }
    }

    // ── The bilinear pass keeps stretching: no bars ──
    {
        ColorConvert conv;
        std::string error;
        CHECK(conv.init(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 64, 32, 32, 32,
                        ColorConvert::Chroma::C420, false, ScaleFilter::Bilinear, error));
        CHECK(!conv.letterboxed());
    }
#endif
}
