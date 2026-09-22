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

#include <QElapsedTimer>
#include <QTimer>
#include <QWindow>

#include <rhi/qrhi.h>

#include <memory>

/// The "Neon Core": a torus knot of hexagonal panels, cyan and magenta edges,
/// wrapped in shells of the same knot and floating in a volumetric glow above a
/// synthwave grid — rendered on ONE chosen GPU, as fast as that GPU can go.
///
/// The swap chain does not wait for vsync and the frames in flight stay queued,
/// so the GPU's queue is always full of this window's work: the situation a
/// game that takes the whole GPU creates for the encoder.
///
/// One knob, the level, sets the cost: the glow's raymarch steps per pixel
/// (pixel load) and the number of knot shells (geometry load) both grow with it.
class RenderWindow : public QWindow
{
    Q_OBJECT

public:
    RenderWindow(const GpuEntry& gpu, QVulkanInstance* vk);
    ~RenderWindow() override;

    void setLevel(double level) { m_level = level; }
    /// Stops submitting at once and frees the device. Nothing is rendered
    /// after this returns; the window can then be deleted.
    void shutdown();
    /// The graphics API and adapter QRhi actually opened, once running.
    QString deviceText() const { return m_deviceText; }

signals:
    /// A frame was submitted; `gpuMs` is the GPU time of the last one the GPU
    /// finished (0 while unknown).
    void frameDone(double gpuMs);
    /// The device could not be created or was lost.
    void failed(const QString& reason, bool deviceLost);
    void started(const QString& deviceText);

protected:
    void exposeEvent(QExposeEvent*) override;
    bool event(QEvent* e) override;

private:
    bool init();
    void renderFrame();
    void releaseResources();

    GpuEntry m_gpu;
    QVulkanInstance* m_vk = nullptr;
    QString m_deviceText;
    double m_level = 40.0;
    bool m_initialized = false;
    bool m_stopped = false;
    QTimer m_loop;
    QElapsedTimer m_clock;

    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiSwapChain> m_sc;
    std::unique_ptr<QRhiRenderBuffer> m_ds;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rp;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiBuffer> m_ibuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_skyPipe;
    std::unique_ptr<QRhiGraphicsPipeline> m_knotPipe;
    quint32 m_indexCount = 0;
    QRhiResourceUpdateBatch* m_initialUpdates = nullptr;
};
