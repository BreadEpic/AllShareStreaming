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

#import <Metal/Metal.h>

// Apple silicon has one GPU, and Qt's Metal backend takes the system default
// device: the list is that device, named the way the encoder's would be.
QList<GpuEntry> enumerateGpus(QVulkanInstance*)
{
    QList<GpuEntry> out;
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) return out;
        GpuEntry gpu;
        gpu.name = QString::fromNSString(dev.name);
        gpu.vramBytes = dev.recommendedMaxWorkingSetSize;
        out.append(gpu);
        [dev release];
    }
    return out;
}
