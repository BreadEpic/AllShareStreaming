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

#include "Bench.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace bench {

/// One GPU with a D3D11 device open on it — the same way the host opens one
/// (11.1 then 11.0, BGRA support, the adapter named so D3D_DRIVER_TYPE_UNKNOWN).
struct Gpu
{
    GpuInfo info;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
};

/// Every hardware adapter of the machine (the Basic Render Driver is left
/// out, as the host leaves it out), each with its device. One that refuses a
/// device is reported in `skipped` and left out.
std::vector<Gpu> enumerateGpus(std::vector<std::string>& skipped, std::string& error);

} // namespace bench
