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
#include "LoadController.h"
#include "ThermalGuard.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QVBoxLayout;
class RenderWindow;

/// A run never lasts longer than this, whatever is asked: the tool drives a GPU
/// flat out, and a minute is enough for a stream to show how it copes.
constexpr int kMaxRunSeconds = 60;
/// The highest limit `--max-temp` accepts.
constexpr double kMaxTempCeiling = 90.0;
constexpr double kDefaultMaxTemp = 85.0;

struct Options
{
    QString gpu;
    bool autostart = false;
    double targetFps = 45.0;
    double level = 0.0; // 0: calibrate
    double maxTemp = kDefaultMaxTemp;
    int duration = kMaxRunSeconds;
    double calibrate = 8.0;
    QString json;
    bool fullscreen = false;
    bool allowNoSensor = false;
};

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    MainWindow(const Options& options, QVulkanInstance* vk);
    ~MainWindow() override;

    /// For --autostart: begins the run as soon as the window is up.
    void startRun();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void stopRun(const QString& reason, const QString& detail = QString());
    void onFrame(double gpuMs);
    void onTick();
    void onThermal();
    void writeJson(const QJsonObject& line);
    void setSliderLevel(double level);
    double sliderLevel() const;
    void refuse(const QString& reason, const QString& text);
    void placeOnScreenOf(const GpuEntry& gpu);

    Options m_options;
    QVulkanInstance* m_vk = nullptr;
    QList<GpuEntry> m_gpus;

    QComboBox* m_gpuBox = nullptr;
    QPushButton* m_startButton = nullptr;
    QCheckBox* m_autoBox = nullptr;
    QCheckBox* m_noSensorBox = nullptr;
    QSlider* m_levelSlider = nullptr;
    QLabel* m_fpsLabel = nullptr;
    QLabel* m_gpuLabel = nullptr;
    QLabel* m_levelLabel = nullptr;
    QLabel* m_tempLabel = nullptr;
    QLabel* m_timeLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_banner = nullptr;
    QVBoxLayout* m_viewLayout = nullptr;
    QPointer<QWidget> m_container;
    QPointer<RenderWindow> m_render;

    std::unique_ptr<ThermalGuard> m_guard;
    LoadController m_controller;
    QTimer m_tick;
    QTimer m_thermal;
    QTimer m_deadline;
    QElapsedTimer m_runClock;
    QFile m_jsonFile;

    bool m_running = false;
    bool m_wasCalibrating = false;
    int m_duration = kMaxRunSeconds;
    double m_limit = kDefaultMaxTemp;
    int m_exitCode = 0;
    // Per-second counters, and the totals of the run.
    int m_frames = 0;
    double m_gpuMsSum = 0.0;
    int m_gpuMsCount = 0;
    qint64 m_lastTickNs = 0;
    // Frames since the level was frozen: the calibration's first seconds run
    // far above the target and would make a whole-run mean meaningless.
    qint64 m_lockedFrames = 0;
    qint64 m_lockedSinceNs = -1;
    double m_peakCelsius = -1.0;
    ThermalReading m_lastReading;
};
