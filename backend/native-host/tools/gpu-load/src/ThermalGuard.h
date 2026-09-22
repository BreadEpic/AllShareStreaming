/*
 * MoonlightWeb — native capture & encoding engine: GPU load tool.
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

#include "GpuList.h"

#include <QString>

#include <memory>

/// One look at the GPU's heat. On Windows and Linux a temperature; macOS has no
/// public GPU sensor, so there it is the system's thermal state
/// (NSProcessInfo: 0 nominal, 1 fair, 2 serious, 3 critical) and `celsius`
/// stays unset.
struct ThermalReading
{
    bool ok = false;
    double celsius = -1.0;
    int state = -1;
};

/// Where the temperature of one GPU is read. Windows: D3DKMT's adapter perf
/// data (what Task Manager shows, any vendor). Linux: the GPU's hwmon, or NVML
/// for the NVIDIA driver. macOS: the thermal state.
class ThermalGuard
{
public:
    explicit ThermalGuard(const GpuEntry& gpu);
    ~ThermalGuard();

    /// Whether a reading answered when the guard was built. Without one, a run
    /// has no heat check at all — see `--allow-no-sensor`.
    bool hasSensor() const;
    /// Which sensor answers, for the window and the JSON log.
    QString source() const;
    /// The temperature the driver says the GPU must stay under, 0 if unknown.
    double deviceLimitCelsius() const;

    ThermalReading read();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
