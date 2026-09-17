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

#include "Adapters.h"

#include <iterator>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace bench {

namespace {

std::string narrow(const wchar_t* s)
{
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

const char* featureLevelName(D3D_FEATURE_LEVEL fl)
{
    switch (fl) {
    case D3D_FEATURE_LEVEL_11_1: return "11.1";
    case D3D_FEATURE_LEVEL_11_0: return "11.0";
    default: return "other";
    }
}

} // namespace

std::vector<Gpu> enumerateGpus(std::vector<std::string>& skipped, std::string& error)
{
    std::vector<Gpu> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        error = "DXGI is unavailable";
        return out;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        // An indirect display driver (the Parsec and the test VDD on
        // DualRTX, 17/09/2026) shows up as a second adapter carrying its
        // render GPU's description under its own LUID, without outputs of
        // its own. One device per silicon is what the bench wants: same
        // vendor, device and name is the same card.
        const uint64_t luid =
            (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
            static_cast<uint64_t>(desc.AdapterLuid.LowPart);
        const std::string name = narrow(desc.Description);
        bool seen = false;
        for (const Gpu& g : out) {
            seen = seen || g.info.luid == luid ||
                   (g.info.vendorId == desc.VendorId && g.info.deviceId == desc.DeviceId &&
                    g.info.name == name);
        }
        if (seen) {
            std::ostringstream why;
            why << name << " listed again by DXGI (LUID 0x" << std::hex << luid
                << "), taken as the same card";
            skipped.push_back(why.str());
            continue;
        }

        Gpu gpu;
        gpu.adapter = adapter;
        gpu.info.index = static_cast<int>(out.size());
        gpu.info.name = name;
        gpu.info.vendorId = desc.VendorId;
        gpu.info.deviceId = desc.DeviceId;
        gpu.info.vramMb = desc.DedicatedVideoMemory / (1024 * 1024);
        gpu.info.luid = luid;

        // The user-mode driver version, as DXGI reports it: no registry.
        LARGE_INTEGER umd = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            std::ostringstream v;
            v << HIWORD(umd.HighPart) << '.' << LOWORD(umd.HighPart) << '.' << HIWORD(umd.LowPart)
              << '.' << LOWORD(umd.LowPart);
            gpu.info.driver = v.str();
        }

        // An adapter with an output attached is one that drives a display.
        ComPtr<IDXGIOutput> output;
        gpu.info.displayGpu =
            adapter->EnumOutputs(0, output.GetAddressOf()) != DXGI_ERROR_NOT_FOUND;

        const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL obtained = {};
        HRESULT hr = ::D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            wanted, static_cast<UINT>(std::size(wanted)), D3D11_SDK_VERSION,
            gpu.device.ReleaseAndGetAddressOf(), &obtained, gpu.context.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            // Once more without the BGRA flag: the bench uses RGBA only, and
            // a driver mid-recovery has been seen refusing the first form.
            const HRESULT first = hr;
            hr = ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, wanted,
                                     static_cast<UINT>(std::size(wanted)), D3D11_SDK_VERSION,
                                     gpu.device.ReleaseAndGetAddressOf(), &obtained,
                                     gpu.context.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                std::ostringstream why;
                why << gpu.info.name << ": no D3D11 device (0x" << std::hex << first
                    << ", without BGRA 0x" << hr << ")";
                skipped.push_back(why.str());
                continue;
            }
        }
        gpu.info.featureLevel = featureLevelName(obtained);

        // Whether min16float means anything to this driver. Without it the
        // fp16 candidates run at fp32 and their figures say nothing new.
        D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT minPrecision = {};
        if (SUCCEEDED(gpu.device->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT,
                                                      &minPrecision, sizeof(minPrecision)))) {
            gpu.info.fp16 =
                (minPrecision.PixelShaderMinPrecision & D3D11_SHADER_MIN_PRECISION_16_BIT) != 0 &&
                (minPrecision.AllOtherShaderStagesMinPrecision &
                 D3D11_SHADER_MIN_PRECISION_16_BIT) != 0;
        }
        out.push_back(std::move(gpu));
    }
    if (out.empty() && error.empty()) error = "no GPU accepted a D3D11 device";
    return out;
}

} // namespace bench
