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

#include <d3d11.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

namespace bench {

/// GPU-side timing of one pass: a TIMESTAMP query before and after each run,
/// a DISJOINT query over the batch so a clock change under it is detected and
/// the batch thrown away rather than counted. Nothing of the CPU is in the
/// figure — no readback, no Present, no Map.
class GpuTimer
{
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, int batchSize,
              std::string& error);

    /// Runs @p pass `batchSize` times under the queries and appends each
    /// run's duration in microseconds to @p outUs. Returns false — appending
    /// nothing — when the batch was disjoint. Blocks until the GPU is done.
    bool measureBatch(const std::function<void()>& pass, std::vector<double>& outUs);

    /// The fallback for work the 3D timestamps do not see (the video
    /// processor): CPU clock from the call to the GPU's completion event,
    /// one run at a time. Includes the submission; never disjoint.
    void measureBatchWall(const std::function<void()>& pass, std::vector<double>& outUs);

    /// Flush and block until everything issued so far has completed.
    void drain();

private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
    Microsoft::WRL::ComPtr<ID3D11Query> m_Disjoint;
    Microsoft::WRL::ComPtr<ID3D11Query> m_Event;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> m_Begin;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> m_End;
    int m_BatchSize = 0;
};

/// Trimmed mean (the same share cut off each end), median, p95, min.
TimeStats computeStats(std::vector<double> us, double trimEachSide, int discarded);

} // namespace bench
