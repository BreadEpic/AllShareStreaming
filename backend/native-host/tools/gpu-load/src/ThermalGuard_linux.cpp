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

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <dlfcn.h>

// The DRM card is found by PCI vendor and device id, the pair Vulkan reports.
// amdgpu, i915 and xe publish the temperature in the card's hwmon (millidegrees);
// NVIDIA's proprietary driver publishes none, and NVML answers for it instead.
namespace {

QString readText(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(f.readAll()).trimmed();
}

// Just the four NVML entry points the guard needs, resolved at run time: the
// tool must start on a machine without the NVIDIA driver.
using NvmlInit = int (*)();
using NvmlShutdown = int (*)();
using NvmlByBus = int (*)(const char*, void**);
using NvmlTemp = int (*)(void*, int, unsigned int*);

} // namespace

struct ThermalGuard::Impl
{
    QString hwmonInput;
    void* nvml = nullptr;
    void* nvmlDevice = nullptr;
    NvmlTemp nvmlTemp = nullptr;
    NvmlShutdown nvmlShutdown = nullptr;
    bool sensor = false;
};

ThermalGuard::ThermalGuard(const GpuEntry& gpu)
    : d(std::make_unique<Impl>())
{
    QString devicePath;
    const QDir drm("/sys/class/drm");
    for (const QString& card : drm.entryList({"card*"}, QDir::Dirs | QDir::System)) {
        if (card.contains('-')) continue; // connectors, e.g. card1-DP-1
        const QString dev = drm.filePath(card) + "/device";
        bool okV = false, okD = false;
        const uint vendor = readText(dev + "/vendor").toUInt(&okV, 16);
        const uint device = readText(dev + "/device").toUInt(&okD, 16);
        if (okV && okD && vendor == gpu.vendorId && device == gpu.deviceId) {
            devicePath = dev;
            break;
        }
    }
    if (!devicePath.isEmpty()) {
        const QDir hwmon(devicePath + "/hwmon");
        for (const QString& h : hwmon.entryList({"hwmon*"}, QDir::Dirs | QDir::System)) {
            const QString input = hwmon.filePath(h) + "/temp1_input";
            if (QFile::exists(input)) {
                d->hwmonInput = input;
                break;
            }
        }
    }
    if (d->hwmonInput.isEmpty() && gpu.vendorId == 0x10de && !devicePath.isEmpty()) {
        d->nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW);
        if (d->nvml) {
            auto init = reinterpret_cast<NvmlInit>(dlsym(d->nvml, "nvmlInit_v2"));
            auto byBus =
                reinterpret_cast<NvmlByBus>(dlsym(d->nvml, "nvmlDeviceGetHandleByPciBusId_v2"));
            d->nvmlTemp = reinterpret_cast<NvmlTemp>(dlsym(d->nvml, "nvmlDeviceGetTemperature"));
            d->nvmlShutdown = reinterpret_cast<NvmlShutdown>(dlsym(d->nvml, "nvmlShutdown"));
            // The device symlink ends in the bus id, e.g. 0000:01:00.0.
            const QByteArray bus =
                QFileInfo(devicePath).canonicalFilePath().section('/', -1).toLatin1();
            if (init && byBus && d->nvmlTemp && init() == 0 &&
                byBus(bus.constData(), &d->nvmlDevice) != 0)
                d->nvmlDevice = nullptr;
        }
    }
    d->sensor = read().ok;
}

ThermalGuard::~ThermalGuard()
{
    if (d->nvml) {
        if (d->nvmlShutdown) d->nvmlShutdown();
        dlclose(d->nvml);
    }
}

bool ThermalGuard::hasSensor() const
{
    return d->sensor;
}

QString ThermalGuard::source() const
{
    if (!d->sensor) return QStringLiteral("none");
    return d->nvmlDevice ? QStringLiteral("NVML") : QStringLiteral("hwmon ") + d->hwmonInput;
}

double ThermalGuard::deviceLimitCelsius() const
{
    return 0.0;
}

ThermalReading ThermalGuard::read()
{
    ThermalReading r;
    if (!d->hwmonInput.isEmpty()) {
        bool ok = false;
        const int milli = readText(d->hwmonInput).toInt(&ok);
        if (ok && milli > 0) {
            r.ok = true;
            r.celsius = milli / 1000.0;
        }
    } else if (d->nvmlDevice) {
        unsigned int c = 0;
        if (d->nvmlTemp(d->nvmlDevice, 0 /* NVML_TEMPERATURE_GPU */, &c) == 0 && c > 0) {
            r.ok = true;
            r.celsius = c;
        }
    }
    return r;
}
