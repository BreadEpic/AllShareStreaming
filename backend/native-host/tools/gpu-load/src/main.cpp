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

// mw-gpu-load — a game-like load on ONE GPU, to see how the encoder copes.
// See tools/gpu-load/CMakeLists.txt for the why and README.md for the bench.

#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
#include <QVulkanInstance>
#include <rhi/qrhi_platform.h>
#endif

#include <cstdio>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("mw-gpu-load"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Spins a neon 3D scene tuned to ~45 fps on one GPU, for at most 60 s, and stops at "
        "once when that GPU gets too hot."));
    parser.addHelpOption();
    const QCommandLineOption gpuOpt(
        "gpu", "GPU: index, LUID (high:low or low) or part of the name.", "gpu");
    const QCommandLineOption listOpt("list", "Print the GPUs and exit.");
    const QCommandLineOption autoOpt("autostart", "Start at once, and quit when the run ends.");
    const QCommandLineOption fpsOpt("target-fps", "Frame rate to calibrate to (45).", "fps", "45");
    const QCommandLineOption levelOpt("level",
                                      "Fixed load level, no calibration (a level an "
                                      "earlier run printed).",
                                      "level");
    const QCommandLineOption tempOpt(
        "max-temp", "Stop at this GPU temperature, °C (85, at most 90).", "celsius", "85");
    const QCommandLineOption durOpt("duration", "Run length in seconds (60, never more).",
                                    "seconds", "60");
    const QCommandLineOption calOpt("calibrate", "Calibration time in seconds (8).", "seconds",
                                    "8");
    const QCommandLineOption jsonOpt("json", "Write one JSON line per second, and the verdict.",
                                     "file");
    const QCommandLineOption fullOpt("fullscreen", "Full screen, like a game.");
    const QCommandLineOption noSensorOpt(
        "allow-no-sensor", "Run even when no temperature sensor answers (60 s cap only).");
    // A bench starts the tool from a script, and a window started that way on a
    // desk somebody is using does not get the foreground: it rendered behind a
    // browser, and the stream carried the browser (22/09/2026).
    const QCommandLineOption topOpt("topmost", "Keep the window above every other one.");
    parser.addOptions({gpuOpt, listOpt, autoOpt, fpsOpt, levelOpt, tempOpt, durOpt, calOpt, jsonOpt,
                       fullOpt, noSensorOpt, topOpt});
    parser.process(app);

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    QVulkanInstance vulkan;
    vulkan.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (!vulkan.create()) {
        std::fprintf(stderr, "mw-gpu-load: no Vulkan instance (%d)\n", int(vulkan.errorCode()));
        return 3;
    }
    QVulkanInstance* vk = &vulkan;
#else
    QVulkanInstance* vk = nullptr;
#endif

    if (parser.isSet(listOpt)) {
        const QList<GpuEntry> gpus = enumerateGpus(vk);
        for (int i = 0; i < gpus.size(); ++i)
            std::printf("%d\t%s\n", i, gpus[i].label().toUtf8().constData());
        std::fflush(stdout);
        return 0;
    }

    Options options;
    options.gpu = parser.value(gpuOpt);
    options.autostart = parser.isSet(autoOpt);
    options.targetFps = qBound(5.0, parser.value(fpsOpt).toDouble(), 240.0);
    options.level = parser.value(levelOpt).toDouble();
    options.maxTemp = qMin(parser.value(tempOpt).toDouble(), kMaxTempCeiling);
    if (options.maxTemp <= 0.0) options.maxTemp = kDefaultMaxTemp;
    options.duration = qBound(1, parser.value(durOpt).toInt(), kMaxRunSeconds);
    options.calibrate = qBound(1.0, parser.value(calOpt).toDouble(), 30.0);
    options.json = parser.value(jsonOpt);
    options.fullscreen = parser.isSet(fullOpt);
    options.allowNoSensor = parser.isSet(noSensorOpt);

    MainWindow window(options, vk);
    if (parser.isSet(topOpt)) window.setWindowFlag(Qt::WindowStaysOnTopHint);
    if (options.fullscreen)
        window.showFullScreen();
    else
        window.show();
    if (options.autostart) QTimer::singleShot(0, &window, &MainWindow::startRun);
    return app.exec();
}
