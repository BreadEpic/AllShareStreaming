/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "convert/windows/d3d12/ComputeConvert.h"

#include "convert/windows/ColorConvert.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mw::native;
using Microsoft::WRL::ComPtr;

namespace {

struct Planes
{
    std::vector<uint8_t> luma;   // w × h
    std::vector<uint8_t> chroma; // (w/2 × h/2) × 2
    int w = 0;
    int h = 0;
};

// A desktop in miniature: a colour ramp, a block of 1-pixel checkerboard (what
// a resample gets wrong first) and a block of saturated primaries (what a
// matrix gets wrong first).
std::vector<uint8_t> picture(int w, int h)
{
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            if (x < w / 3) {
                p[0] = static_cast<uint8_t>(x * 255 / (w / 3));
                p[1] = static_cast<uint8_t>(y * 255 / h);
                p[2] = static_cast<uint8_t>(255 - p[0]);
            } else if (x < 2 * w / 3) {
                p[0] = p[1] = p[2] = ((x + y) & 1) ? 255 : 0;
            } else {
                const int band = y * 4 / h;
                p[0] = band == 0 ? 255 : 0;
                p[1] = band == 1 ? 255 : 0;
                p[2] = band == 2 ? 255 : 40;
            }
            p[3] = 255;
        }
    }
    return px;
}

capture::CursorState arrow()
{
    capture::CursorState cursor;
    cursor.visible = true;
    cursor.x = 9;
    cursor.y = 5;
    cursor.width = 12;
    cursor.height = 12;
    cursor.shapeVersion = 1;
    cursor.pixels.assign(12 * 12 * 4, 0);
    cursor.invert.assign(12 * 12, 0);
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x <= y && x < 12; ++x) {
            uint8_t* p = &cursor.pixels[(static_cast<size_t>(y) * 12 + x) * 4];
            p[0] = 30;
            p[1] = 200;
            p[2] = 250;
            p[3] = x == y ? 128 : 255;
        }
        cursor.invert[static_cast<size_t>(y) * 12 + 11] = 255; // an inverting column
    }
    return cursor;
}

int maxDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if (a.size() != b.size() || a.empty()) return 255;
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
    return worst;
}

// ── The D3D11 side ──────────────────────────────────────────────────────────

bool convert11(ID3D11Device* device, ID3D11DeviceContext* context, const std::vector<uint8_t>& px,
               int w, int h, int outW, int outH, convert::ScaleFilter filter,
               const capture::CursorState& cursor, Planes& out)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA seed = {px.data(), static_cast<UINT>(w * 4), 0};
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(device->CreateTexture2D(&desc, &seed, source.GetAddressOf()))) return false;

    convert::ColorConvert conv;
    std::string error;
    if (!conv.init(device, DXGI_FORMAT_B8G8R8A8_UNORM, w, h, outW, outH,
                   convert::ColorConvert::Chroma::C420, false, filter, error) ||
        !conv.convert(source.Get(), cursor, convert::CursorDraw{}, error)) {
        std::fprintf(stderr, "  D3D11: %s\n", error.c_str());
        return false;
    }

    conv.output()->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return false;
    context->CopyResource(staging.Get(), conv.output());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    out.w = conv.outputWidth();
    out.h = conv.outputHeight();
    const auto* base = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < out.h; ++y)
        out.luma.insert(out.luma.end(), base + static_cast<size_t>(y) * mapped.RowPitch,
                        base + static_cast<size_t>(y) * mapped.RowPitch + out.w);
    for (int y = 0; y < out.h / 2; ++y)
        out.chroma.insert(out.chroma.end(), base + static_cast<size_t>(out.h + y) * mapped.RowPitch,
                          base + static_cast<size_t>(out.h + y) * mapped.RowPitch + out.w);
    context->Unmap(staging.Get(), 0);
    return true;
}

// ── The D3D12 side ──────────────────────────────────────────────────────────

ComPtr<ID3D12Resource> buffer12(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> out;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    type == D3D12_HEAP_TYPE_UPLOAD
                                        ? D3D12_RESOURCE_STATE_GENERIC_READ
                                        : D3D12_RESOURCE_STATE_COPY_DEST,
                                    nullptr, IID_PPV_ARGS(&out));
    return out;
}

D3D12_RESOURCE_BARRIER move12(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                              D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}

ComPtr<ID3D12Resource> source12(platform::D3d12Context& context, const std::vector<uint8_t>& px,
                                int w, int h)
{
    ID3D12Device* device = context.device();
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(w);
    desc.Height = static_cast<UINT>(h);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&texture))))
        return nullptr;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    ComPtr<ID3D12Resource> upload = buffer12(device, bytes, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t* mapped = nullptr;
    if (!upload || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return nullptr;
    for (int y = 0; y < h; ++y)
        std::memcpy(mapped + footprint.Offset +
                        static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    px.data() + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
    upload->Unmap(0, nullptr);

    std::string error;
    ID3D12GraphicsCommandList* list = context.begin(error);
    if (!list) return nullptr;
    D3D12_TEXTURE_COPY_LOCATION to = {texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION from = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    from.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    // COMMON, as a surface shared from D3D11 arrives.
    const D3D12_RESOURCE_BARRIER barrier =
        move12(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &barrier);
    if (!context.submitAndWait(error)) return nullptr;
    return texture;
}

bool readback12(platform::D3d12Context& context, convert::ComputeConvert& conv, Planes& out)
{
    ID3D12Device* device = context.device();
    const D3D12_RESOURCE_DESC desc = conv.output()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT planes[2] = {};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 2, 0, planes, nullptr, nullptr, &bytes);
    ComPtr<ID3D12Resource> readback = buffer12(device, bytes, D3D12_HEAP_TYPE_READBACK);
    if (!readback) return false;

    std::string error;
    ID3D12GraphicsCommandList* list = context.begin(error);
    if (!list) return false;
    D3D12_RESOURCE_BARRIER barrier =
        move12(conv.output(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);
    for (UINT plane = 0; plane < 2; ++plane) {
        D3D12_TEXTURE_COPY_LOCATION to = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint = planes[plane];
        D3D12_TEXTURE_COPY_LOCATION from = {conv.output(),
                                            D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        from.SubresourceIndex = plane;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    barrier = move12(conv.output(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &barrier);
    if (!context.submitAndWait(error)) return false;

    uint8_t* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    out.w = conv.outputWidth();
    out.h = conv.outputHeight();
    for (int y = 0; y < out.h; ++y) {
        const uint8_t* row =
            mapped + planes[0].Offset + static_cast<size_t>(y) * planes[0].Footprint.RowPitch;
        out.luma.insert(out.luma.end(), row, row + out.w);
    }
    for (int y = 0; y < out.h / 2; ++y) {
        const uint8_t* row =
            mapped + planes[1].Offset + static_cast<size_t>(y) * planes[1].Footprint.RowPitch;
        out.chroma.insert(out.chroma.end(), row, row + out.w);
    }
    readback->Unmap(0, nullptr);
    return true;
}

} // namespace
#endif

// The D3D12 converter against the D3D11 one, both on WARP so a headless runner
// can hold them to each other: whichever pipeline carries a stream, the viewer
// gets the same picture.
void run_compute_convert_tests()
{
#if defined(_WIN32)
    using ScaleFilter = convert::ScaleFilter;

    SECTION("ComputeConvert: the same picture as ColorConvert, on WARP");

    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    platform::D3d12Context context12;
    std::string error;
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, wanted, 2, D3D11_SDK_VERSION,
                                   device11.GetAddressOf(), nullptr, context11.GetAddressOf())) ||
        FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        !context12.init(warp.Get(), error)) {
        std::fprintf(stderr, "  no WARP device for both APIs here (%s) — skipped\n", error.c_str());
        return;
    }

    struct Case
    {
        const char* name;
        int w, h, outW, outH;
        ScaleFilter filter;
        bool cursor;
        bool letterboxed;
        int tolerance;
    };
    // One code of slack where both do the same arithmetic; two on the resample
    // path, where D3D11's render target encodes sRGB in hardware and compute
    // spells the formula.
    const Case cases[] = {
        {"1:1", 96, 64, 96, 64, ScaleFilter::Lanczos2, false, false, 1},
        {"1:1 with the pointer", 96, 64, 96, 64, ScaleFilter::Lanczos2, true, false, 1},
        {"bilinear to 2/3", 96, 64, 64, 42, ScaleFilter::Bilinear, false, false, 1},
        {"Lanczos-2 to 2/3", 96, 64, 64, 42, ScaleFilter::Lanczos2, false, false, 2},
        {"Lanczos-2 to 2/3 with the pointer", 96, 64, 64, 42, ScaleFilter::Lanczos2, true, false,
         2},
        {"Lanczos-2, letterboxed", 96, 48, 48, 48, ScaleFilter::Lanczos2, true, true, 2},
    };
    for (const Case& c : cases) {
        const std::vector<uint8_t> px = picture(c.w, c.h);
        const capture::CursorState cursor = c.cursor ? arrow() : capture::CursorState{};

        Planes reference;
        CHECK(convert11(device11.Get(), context11.Get(), px, c.w, c.h, c.outW, c.outH, c.filter,
                        cursor, reference));

        convert::ComputeConvert conv;
        ComPtr<ID3D12Resource> source = source12(context12, px, c.w, c.h);
        CHECK(source != nullptr);
        const bool ready = source && conv.init(context12, DXGI_FORMAT_B8G8R8A8_UNORM, c.w, c.h,
                                               c.outW, c.outH, false, c.filter, nullptr, error);
        if (!ready) std::fprintf(stderr, "  %s: %s\n", c.name, error.c_str());
        CHECK(ready);
        if (!ready) continue;
        CHECK(conv.letterboxed() == c.letterboxed);
        // Twice: the second frame reuses the views, the cursor and the states
        // the first one left behind.
        CHECK(conv.convert(source.Get(), cursor, convert::CursorDraw{}, error));
        CHECK(conv.convert(source.Get(), cursor, convert::CursorDraw{}, error));
        Planes got;
        CHECK(readback12(context12, conv, got));

        const int luma = maxDifference(reference.luma, got.luma);
        const int chroma = maxDifference(reference.chroma, got.chroma);
        if (luma > c.tolerance || chroma > c.tolerance)
            std::fprintf(stderr, "  %s: luma off by %d, chroma by %d (allowed %d)\n", c.name, luma,
                         chroma, c.tolerance);
        CHECK(luma <= c.tolerance);
        CHECK(chroma <= c.tolerance);
    }

    SECTION("ComputeConvert: dropping the resample keeps the output texture");
    {
        const std::vector<uint8_t> px = picture(96, 64);
        convert::ComputeConvert conv;
        ComPtr<ID3D12Resource> source = source12(context12, px, 96, 64);
        CHECK(conv.init(context12, DXGI_FORMAT_B8G8R8A8_UNORM, 96, 64, 64, 42, false,
                        ScaleFilter::Lanczos2, nullptr, error));
        ID3D12Resource* before = conv.output();
        CHECK(conv.convert(source.Get(), capture::CursorState{}, convert::CursorDraw{}, error));
        CHECK(conv.dropResample());
        CHECK(!conv.dropResample());
        CHECK(conv.scaleFilter() == ScaleFilter::Bilinear);
        CHECK(conv.output() == before);
        CHECK(conv.convert(source.Get(), capture::CursorState{}, convert::CursorDraw{}, error));

        Planes reference, got;
        CHECK(convert11(device11.Get(), context11.Get(), px, 96, 64, 64, 42, ScaleFilter::Bilinear,
                        capture::CursorState{}, reference));
        CHECK(readback12(context12, conv, got));
        CHECK(maxDifference(reference.luma, got.luma) <= 1);
    }

    SECTION("ComputeConvert: between D3D11 and D3D12 on a real GPU (the hybrid pipeline)");
    {
        // What WARP cannot show: a D3D11 surface opened in D3D12 the way
        // Desktop Duplication's is, and the converted picture read back from
        // the D3D11 side, where a D3D11 encoder would take it. Every hardware
        // adapter of the machine, since they do not all agree.
        ComPtr<IDXGIAdapter1> adapter;
        bool any = false;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
             ++i, adapter.Reset()) {
            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            char name[128] = {};
            ::WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr,
                                  nullptr);

            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> immediate;
            platform::D3d12Context context;
            if (FAILED(::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, wanted, 2,
                                           D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                                           immediate.GetAddressOf())) ||
                !context.init(device.Get(), error)) {
                std::fprintf(stderr, "  %s: no D3D11 + D3D12 pair (%s) — skipped\n", name,
                             error.c_str());
                continue;
            }

            const int w = 320, h = 180;
            const std::vector<uint8_t> px = picture(w, h);
            D3D11_TEXTURE2D_DESC shared = {};
            shared.Width = w;
            shared.Height = h;
            shared.MipLevels = 1;
            shared.ArraySize = 1;
            shared.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            shared.SampleDesc.Count = 1;
            shared.Usage = D3D11_USAGE_DEFAULT;
            shared.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            shared.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
            D3D11_SUBRESOURCE_DATA seed = {px.data(), static_cast<UINT>(w * 4), 0};
            ComPtr<ID3D11Texture2D> captured;
            if (FAILED(device->CreateTexture2D(&shared, &seed, captured.GetAddressOf()))) {
                std::fprintf(stderr, "  %s: no shareable D3D11 texture — skipped\n", name);
                continue;
            }
            immediate->Flush();

            ID3D12Resource* opened = context.open(captured.Get(), error);
            convert::ComputeConvert conv;
            if (!opened || !conv.init(context, DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 212, 120, false,
                                      ScaleFilter::Lanczos2, device.Get(), error)) {
                // A driver that cannot do this keeps the D3D11 pipeline; that is
                // a finding to read in the log, not a broken build.
                std::fprintf(stderr, "  %s: %s — skipped\n", name, error.c_str());
                continue;
            }
            any = true;
            CHECK(context.open(captured.Get(), error) == opened); // opened once
            CHECK(conv.outputD3d11() != nullptr);
            CHECK(conv.convert(opened, arrow(), convert::CursorDraw{}, error));

            // Read back on the D3D11 side only, which is the side that matters
            // here. (Not through readback12 as well: Intel's driver — 21/09/2026,
            // Arc A380 — crashes inside CopyTextureRegion when a COMPUTE list
            // copies this shared planar texture to a buffer. Nothing in the host
            // does that; the encoder reads the texture, nobody copies it out.)
            Planes through11;
            through11.w = conv.outputWidth();
            through11.h = conv.outputHeight();
            D3D11_TEXTURE2D_DESC staged = {};
            conv.outputD3d11()->GetDesc(&staged);
            staged.Usage = D3D11_USAGE_STAGING;
            staged.BindFlags = 0;
            staged.MiscFlags = 0;
            staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            const bool read =
                SUCCEEDED(device->CreateTexture2D(&staged, nullptr, staging.GetAddressOf())) &&
                (immediate->CopyResource(staging.Get(), conv.outputD3d11()), true) &&
                SUCCEEDED(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            CHECK(read);
            if (read) {
                const auto* base = static_cast<const uint8_t*>(mapped.pData);
                for (int y = 0; y < through11.h * 3 / 2; ++y) {
                    auto& plane = y < through11.h ? through11.luma : through11.chroma;
                    plane.insert(plane.end(), base + static_cast<size_t>(y) * mapped.RowPitch,
                                 base + static_cast<size_t>(y) * mapped.RowPitch + through11.w);
                }
                immediate->Unmap(staging.Get(), 0);
            }

            // And it is the picture the D3D11 pipeline makes on that GPU.
            Planes reference;
            CHECK(convert11(device.Get(), immediate.Get(), px, w, h, 212, 120,
                            ScaleFilter::Lanczos2, arrow(), reference));
            const int luma = maxDifference(reference.luma, through11.luma);
            const int chroma = maxDifference(reference.chroma, through11.chroma);
            std::fprintf(stderr,
                         "  %s: shared both ways; against D3D11, luma off by %d, chroma by %d\n",
                         name, luma, chroma);
            CHECK(luma <= 2);
            CHECK(chroma <= 2);
        }
        if (!any) std::fprintf(stderr, "  no GPU here shares between D3D11 and D3D12 — skipped\n");
    }

    SECTION("ComputeConvert: what it refuses");
    {
        convert::ComputeConvert conv;
        CHECK(!conv.init(context12, DXGI_FORMAT_B8G8R8A8_UNORM, 64, 64, 64, 64, true,
                         ScaleFilter::Bilinear, nullptr, error)); // HDR from 8-bit frames
        CHECK(!conv.init(context12, DXGI_FORMAT_R10G10B10A2_UNORM, 64, 64, 64, 64, false,
                         ScaleFilter::Bilinear, nullptr, error));
    }
#endif
}
