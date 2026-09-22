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

#include "GpuList.h"

#if defined(Q_OS_WIN)
#include <dxgi1_6.h>
#include <windows.h>
#elif !defined(Q_OS_MACOS)
#include <QVulkanFunctions>
#include <QVulkanInstance>
#endif

QString GpuEntry::label() const
{
    QString text = name;
    if (vramBytes >= (quint64(1) << 30))
        text += QString(" — %1 GB").arg(double(vramBytes) / double(quint64(1) << 30), 0, 'f', 1);
    else if (vramBytes)
        text += QString(" — %1 MB").arg(vramBytes >> 20);
    if (luidLow || luidHigh) text += QString(" (LUID %1)").arg(luidText());
    if (!outputName.isEmpty()) text += QString(" — %1").arg(outputName);
    return text;
}

QString GpuEntry::luidText() const
{
    return QString("%1:%2").arg(luidHigh).arg(luidLow);
}

#if defined(Q_OS_WIN)

QList<GpuEntry> enumerateGpus(QVulkanInstance*)
{
    QList<GpuEntry> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return out;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter->Release();
            continue;
        }
        GpuEntry gpu;
        gpu.name = QString::fromWCharArray(desc.Description);
        gpu.vramBytes = desc.DedicatedVideoMemory;
        gpu.vendorId = desc.VendorId;
        gpu.deviceId = desc.DeviceId;
        gpu.luidLow = desc.AdapterLuid.LowPart;
        gpu.luidHigh = desc.AdapterLuid.HighPart;
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(0, &output) == S_OK) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(output->GetDesc(&od)))
                gpu.outputName = QString::fromWCharArray(od.DeviceName);
            output->Release();
        }
        adapter->Release();
        // The same board can be listed twice (an indirect display attached to
        // it shows up as a second adapter with its own LUID): one entry is
        // enough, and the first one is the adapter the desktop renders on.
        GpuEntry* seen = nullptr;
        for (GpuEntry& other : out)
            if (other.vendorId == gpu.vendorId && other.deviceId == gpu.deviceId &&
                other.vramBytes == gpu.vramBytes && other.name == gpu.name)
                seen = &other;
        if (!seen)
            out.append(gpu);
        else if (seen->outputName.isEmpty())
            seen->outputName = gpu.outputName;
    }
    factory->Release();
    return out;
}

#elif !defined(Q_OS_MACOS)

QList<GpuEntry> enumerateGpus(QVulkanInstance* vk)
{
    QList<GpuEntry> out;
    if (!vk || !vk->isValid()) return out;
    QVulkanFunctions* f = vk->functions();
    uint32_t count = 0;
    f->vkEnumeratePhysicalDevices(vk->vkInstance(), &count, nullptr);
    QList<VkPhysicalDevice> devices(count);
    f->vkEnumeratePhysicalDevices(vk->vkInstance(), &count, devices.data());
    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties props;
        f->vkGetPhysicalDeviceProperties(dev, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue; // llvmpipe
        GpuEntry gpu;
        gpu.name = QString::fromUtf8(props.deviceName);
        gpu.vendorId = props.vendorID;
        gpu.deviceId = props.deviceID;
        gpu.vkPhysDev = dev;
        VkPhysicalDeviceMemoryProperties mem;
        f->vkGetPhysicalDeviceMemoryProperties(dev, &mem);
        for (uint32_t h = 0; h < mem.memoryHeapCount; ++h)
            if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                gpu.vramBytes = qMax<quint64>(gpu.vramBytes, mem.memoryHeaps[h].size);
        // Vulkan does not order by display; a discrete GPU first is the better
        // guess at "the one that matters" when nothing is asked.
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            out.prepend(gpu);
        else
            out.append(gpu);
    }
    return out;
}

#endif

int findGpu(const QList<GpuEntry>& gpus, const QString& spec)
{
    const QString s = spec.trimmed();
    if (s.isEmpty()) return gpus.isEmpty() ? -1 : 0;
    bool isNumber = false;
    const qlonglong n = s.toLongLong(&isNumber);
    if (isNumber) {
        // A small number is an index; a large one is the low part of a LUID.
        if (n >= 0 && n < gpus.size()) return int(n);
        for (int i = 0; i < gpus.size(); ++i)
            if (gpus[i].luidLow == quint32(n)) return i;
        return -1;
    }
    for (int i = 0; i < gpus.size(); ++i)
        if (gpus[i].luidText() == s) return i;
    for (int i = 0; i < gpus.size(); ++i)
        if (gpus[i].name.compare(s, Qt::CaseInsensitive) == 0) return i;
    for (int i = 0; i < gpus.size(); ++i)
        if (gpus[i].name.contains(s, Qt::CaseInsensitive)) return i;
    return -1;
}
