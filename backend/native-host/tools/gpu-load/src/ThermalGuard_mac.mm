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

#include "ThermalGuard.h"

#import <Foundation/Foundation.h>

// macOS exposes no GPU temperature to a normal process (the SMC keys are
// private, powermetrics wants root). The system's thermal state is public and
// is what the OS itself throttles on: the guard stops at "serious".
struct ThermalGuard::Impl
{};

ThermalGuard::ThermalGuard(const GpuEntry&)
    : d(std::make_unique<Impl>())
{}

ThermalGuard::~ThermalGuard() = default;

bool ThermalGuard::hasSensor() const
{
    return true;
}

QString ThermalGuard::source() const
{
    return QStringLiteral("NSProcessInfo thermal state");
}

double ThermalGuard::deviceLimitCelsius() const
{
    return 0.0;
}

ThermalReading ThermalGuard::read()
{
    ThermalReading r;
    r.ok = true;
    r.state = int([[NSProcessInfo processInfo] thermalState]);
    return r;
}
