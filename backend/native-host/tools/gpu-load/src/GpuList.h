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

#include <QList>
#include <QString>

class QVulkanInstance;

/// A GPU the load can run on, as the OS names it. The name is the one
/// MoonlightWeb reports for the encoder of a display (/api/native/status), so
/// `--gpu "<that name>"` lands the load on the encoder's own GPU.
struct GpuEntry
{
    QString name;
    quint64 vramBytes = 0;
    quint32 vendorId = 0;
    quint32 deviceId = 0;
    // Windows: the DXGI adapter LUID, which is how D3D12 is told which GPU.
    quint32 luidLow = 0;
    qint32 luidHigh = 0;
    // Linux: the Vulkan physical device.
    void* vkPhysDev = nullptr;
    // Windows: the first display this GPU drives (\\.\DISPLAYn, QScreen::name),
    // empty when it drives none. The load window goes there: that display is
    // the one MoonlightWeb captures when it encodes on this GPU, and a window
    // shown on another GPU's display would cost a cross-adapter copy per frame.
    QString outputName;

    QString label() const;
    QString luidText() const;
};

/// Hardware GPUs, the one driving the primary display first (DXGI's order on
/// Windows). Software rasterizers are left out: they load the CPU, not a GPU.
/// `vk` is only used on Linux, where the list comes from Vulkan.
QList<GpuEntry> enumerateGpus(QVulkanInstance* vk);

/// Index of the GPU `spec` names — an index, a LUID ("high:low" or the low part
/// in decimal, as the bench scripts write it) or a case-insensitive part of the
/// name — or -1.
int findGpu(const QList<GpuEntry>& gpus, const QString& spec);
