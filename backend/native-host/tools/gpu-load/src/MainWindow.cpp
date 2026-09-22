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

#include "MainWindow.h"

#include "RenderWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#if defined(Q_OS_WIN)
#include <qscreen_platform.h>
#include <windows.h>
#endif

namespace {

constexpr int kSliderSteps = 1000;

// Exit codes, for the bench scripts.
constexpr int kExitOk = 0;
constexpr int kExitThermal = 2;
constexpr int kExitDevice = 3;
constexpr int kExitRefused = 4;

QString celsiusText(const ThermalReading& r)
{
    if (!r.ok) return QStringLiteral("n/a");
    if (r.state >= 0) {
        static const char* const names[] = {"nominal", "fair", "serious", "critical"};
        return QString::fromLatin1(names[std::clamp(r.state, 0, 3)]);
    }
    return QString("%1 °C").arg(r.celsius, 0, 'f', 1);
}

/// The name GpuList gives a display: its GDI device name on Windows
/// (\\.\DISPLAY1). Qt 6 calls a screen by its monitor's model instead
/// ("M27Q"), so the two never matched, and the window stayed on whatever screen
/// it opened on — a GPU rendering to another GPU's display (22/09/2026).
QString displayName(QScreen* screen)
{
#if defined(Q_OS_WIN)
    if (auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>()) {
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(native->handle(), &info)) return QString::fromWCharArray(info.szDevice);
    }
#endif
    return screen->name();
}

} // namespace

MainWindow::MainWindow(const Options& options, QVulkanInstance* vk)
    : m_options(options)
    , m_vk(vk)
    , m_gpus(enumerateGpus(vk))
{
    setWindowTitle(QStringLiteral("MoonlightWeb GPU load"));
    setStyleSheet(QStringLiteral(
        "QWidget { background: #07090b; color: #e6e9ee; }"
        "QPushButton { background: #fff64a; color: #07090b; font-weight: bold; padding: 4px 14px; }"
        "QPushButton:disabled { background: #555; }"
        "QComboBox { background: #14181d; padding: 3px; }"
        "QLabel#banner { background: #b3122e; color: white; font-weight: bold; padding: 4px; }"));

    m_gpuBox = new QComboBox(this);
    for (const GpuEntry& gpu : m_gpus)
        m_gpuBox->addItem(gpu.label());
    const int wanted = findGpu(m_gpus, m_options.gpu);
    if (wanted >= 0) m_gpuBox->setCurrentIndex(wanted);

    m_startButton = new QPushButton(QStringLiteral("Start"), this);
    m_autoBox = new QCheckBox(QString("Auto %1 fps").arg(m_options.targetFps), this);
    m_autoBox->setChecked(m_options.level <= 0.0);
    m_levelSlider = new QSlider(Qt::Horizontal, this);
    m_levelSlider->setRange(0, kSliderSteps);
    m_levelSlider->setMinimumWidth(160);
    m_levelSlider->setEnabled(!m_autoBox->isChecked());
    setSliderLevel(m_options.level > 0.0 ? m_options.level : 40.0);
    m_noSensorBox = new QCheckBox(QString("No sensor: allow %1 s").arg(kMaxRunSeconds), this);
    m_noSensorBox->setChecked(m_options.allowNoSensor);
    m_musicBox = new QCheckBox(QStringLiteral("Music"), this);
    m_musicBox->setChecked(m_options.music);

    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel(QStringLiteral("GPU"), this));
    controls->addWidget(m_gpuBox, 1);
    controls->addWidget(m_startButton);
    controls->addWidget(m_autoBox);
    controls->addWidget(m_levelSlider);
    controls->addWidget(m_noSensorBox);
    controls->addWidget(m_musicBox);

    m_fpsLabel = new QLabel(this);
    m_gpuLabel = new QLabel(this);
    m_levelLabel = new QLabel(this);
    m_tempLabel = new QLabel(this);
    m_timeLabel = new QLabel(this);
    m_statusLabel = new QLabel(QStringLiteral("Ready."), this);
    auto* readout = new QHBoxLayout;
    for (QLabel* l : {m_fpsLabel, m_gpuLabel, m_levelLabel, m_tempLabel, m_timeLabel})
        readout->addWidget(l);
    readout->addWidget(m_statusLabel, 1);
    // What came from the client, and how the host's own audio fares: a click
    // heard on the client with no underrun here was made on the way.
    m_inputLabel = new QLabel(this);
    m_audioLabel = new QLabel(this);
    auto* checks = new QHBoxLayout;
    checks->addWidget(m_inputLabel);
    checks->addWidget(m_audioLabel, 1);
    updateInputAudio();

    m_banner = new QLabel(this);
    m_banner->setObjectName(QStringLiteral("banner"));
    m_banner->hide();

    m_viewLayout = new QVBoxLayout;
    m_viewLayout->setContentsMargins(0, 0, 0, 0);
    auto* root = new QVBoxLayout(this);
    root->addLayout(controls);
    root->addLayout(readout);
    root->addLayout(checks);
    root->addWidget(m_banner);
    root->addLayout(m_viewLayout, 1);
    resize(1600, 960);

    connect(m_startButton, &QPushButton::clicked, this, [this]() {
        if (m_running)
            stopRun(QStringLiteral("user"));
        else
            startRun();
    });
    connect(m_autoBox, &QCheckBox::toggled, this,
            [this](bool on) { m_levelSlider->setEnabled(!on || m_controller.locked()); });
    connect(m_musicBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_running) return;
        if (on)
            m_music.start();
        else
            m_music.stop();
    });
    connect(m_levelSlider, &QSlider::valueChanged, this, [this]() {
        if (!m_running || m_autoBox->isChecked()) return;
        m_controller.setLevel(sliderLevel());
        if (m_render) m_render->setLevel(m_controller.level());
    });

    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &MainWindow::onTick);
    m_thermal.setInterval(500);
    connect(&m_thermal, &QTimer::timeout, this, &MainWindow::onThermal);
    m_deadline.setSingleShot(true);
    m_deadline.setTimerType(Qt::PreciseTimer);
    connect(&m_deadline, &QTimer::timeout, this, [this]() { stopRun(QStringLiteral("timeout")); });

    if (!m_options.json.isEmpty()) {
        m_jsonFile.setFileName(m_options.json);
        if (!m_jsonFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            m_statusLabel->setText(QString("Cannot write %1.").arg(m_options.json));
    }
    if (m_gpus.isEmpty()) {
        m_startButton->setEnabled(false);
        m_statusLabel->setText(QStringLiteral("No hardware GPU found."));
    } else if (!m_options.gpu.isEmpty() && wanted < 0) {
        m_statusLabel->setText(QString("No GPU matches \"%1\".").arg(m_options.gpu));
    }
}

MainWindow::~MainWindow()
{
    if (m_running) stopRun(QStringLiteral("user"));
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (m_running) stopRun(QStringLiteral("user"));
    e->accept();
}

double MainWindow::sliderLevel() const
{
    const double t = double(m_levelSlider->value()) / kSliderSteps;
    return std::exp(
        std::log(LoadController::kMinLevel) +
        t * (std::log(LoadController::kMaxLevel) - std::log(LoadController::kMinLevel)));
}

void MainWindow::setSliderLevel(double level)
{
    const double t = (std::log(std::max(level, LoadController::kMinLevel)) -
                      std::log(LoadController::kMinLevel)) /
                     (std::log(LoadController::kMaxLevel) - std::log(LoadController::kMinLevel));
    const QSignalBlocker block(m_levelSlider);
    m_levelSlider->setValue(int(std::lround(t * kSliderSteps)));
}

void MainWindow::writeJson(const QJsonObject& line)
{
    if (!m_jsonFile.isOpen()) return;
    m_jsonFile.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
    m_jsonFile.flush();
}

void MainWindow::refuse(const QString& reason, const QString& text)
{
    m_statusLabel->setText(text);
    m_banner->setText(text);
    m_banner->show();
    writeJson({{"event", "refused"}, {"reason", reason}, {"detail", text}});
    if (m_options.autostart) {
        m_exitCode = kExitRefused;
        QTimer::singleShot(0, qApp, [code = m_exitCode]() { QApplication::exit(code); });
    }
}

void MainWindow::placeOnScreenOf(const GpuEntry& gpu)
{
    // On the display that GPU drives, which is the one MoonlightWeb captures
    // when it encodes there. A GPU that drives no display (a headless card, or
    // one only a virtual display is attached to) leaves the window where it is.
    if (gpu.outputName.isEmpty()) return;
    QScreen* target = nullptr;
    for (QScreen* s : QGuiApplication::screens())
        if (displayName(s) == gpu.outputName) target = s;
    if (!target || target == screen()) return;
    const bool full = isFullScreen();
    if (full) showNormal();
    const QRect area = target->availableGeometry();
    resize(size().boundedTo(area.size()));
    move(area.center() - rect().center());
    if (full) showFullScreen();
}

void MainWindow::startRun()
{
    if (m_running) return;
    const int index = m_gpuBox->currentIndex();
    if (index < 0 || index >= m_gpus.size()) {
        refuse(QStringLiteral("no-gpu"), QStringLiteral("No GPU selected."));
        return;
    }
    if (!m_options.gpu.isEmpty() && m_options.autostart && findGpu(m_gpus, m_options.gpu) < 0) {
        refuse(QStringLiteral("no-gpu"), QString("No GPU matches \"%1\".").arg(m_options.gpu));
        return;
    }
    const GpuEntry gpu = m_gpus[index];
    m_banner->hide();

    // The heat check comes first: no sensor, or a GPU already at its limit,
    // and nothing is rendered at all.
    m_guard = std::make_unique<ThermalGuard>(gpu);
    m_limit = std::min(m_options.maxTemp, kMaxTempCeiling);
    if (m_guard->deviceLimitCelsius() > 0.0)
        m_limit = std::min(m_limit, m_guard->deviceLimitCelsius() - 3.0);
    if (!m_guard->hasSensor() && !m_noSensorBox->isChecked()) {
        refuse(QStringLiteral("no-sensor"),
               QString("No temperature sensor answers for %1. Tick \"%2\" to run anyway.")
                   .arg(gpu.name, m_noSensorBox->text()));
        return;
    }
    m_lastReading = m_guard->read();
    m_peakCelsius = m_lastReading.celsius;
    if (m_lastReading.ok && ((m_lastReading.state < 0 && m_lastReading.celsius >= m_limit) ||
                             m_lastReading.state >= 2)) {
        refuse(QStringLiteral("already-hot"), QString("%1 is already at %2 (limit %3 °C).")
                                                  .arg(gpu.name, celsiusText(m_lastReading))
                                                  .arg(m_limit, 0, 'f', 0));
        return;
    }
    if (!m_guard->hasSensor()) {
        m_banner->setText(QString("No temperature sensor for %1: this run has no heat check, "
                                  "only the %2 s limit.")
                              .arg(gpu.name)
                              .arg(kMaxRunSeconds));
        m_banner->show();
    }

    m_duration = std::clamp(m_options.duration, 1, kMaxRunSeconds);
    const bool autoTune = m_autoBox->isChecked();
    m_controller.reset(autoTune ? 40.0 : sliderLevel(), m_options.targetFps,
                       std::min(m_options.calibrate, m_duration * 0.5), autoTune);
    m_wasCalibrating = m_controller.calibrating();
    m_frames = 0;
    m_gpuMsSum = 0.0;
    m_gpuMsCount = 0;
    m_lockedFrames = 0;
    m_lockedSinceNs = m_controller.locked() ? 0 : -1;
    m_exitCode = kExitOk;

    placeOnScreenOf(gpu);
    auto* render = new RenderWindow(gpu, m_vk);
    render->setLevel(m_controller.level());
    m_render = render;
    m_container = QWidget::createWindowContainer(render, this);
    m_viewLayout->addWidget(m_container);
    connect(render, &RenderWindow::frameDone, this, &MainWindow::onFrame);
    render->setPulseSource([this]() { return m_music.kickPulse(); });
    connect(render, &RenderWindow::arrowPressed, this,
            [this](int direction) { m_music.blip(direction); });
    connect(render, &RenderWindow::failed, this, [this](const QString& reason, bool lost) {
        m_exitCode = kExitDevice;
        stopRun(lost ? QStringLiteral("device-lost") : QStringLiteral("device-error"), reason);
    });
    connect(render, &RenderWindow::started, this, [this](const QString& device) {
        m_runClock.start();
        m_lastTickNs = 0;
        m_statusLabel->setText(device);
        // The keys go to the knot, not to a button of the bar above it.
        if (m_container) m_container->setFocus();
        if (m_render) m_render->requestActivate();
    });

    m_running = true;
    m_startButton->setText(QStringLiteral("Stop"));
    m_gpuBox->setEnabled(false);
    m_autoBox->setEnabled(false);
    m_noSensorBox->setEnabled(false);
    m_levelSlider->setEnabled(!autoTune);
    m_runClock.start();
    m_deadline.start(m_duration * 1000);
    m_tick.start();
    m_thermal.start();
    if (m_musicBox->isChecked()) m_music.start();
    writeJson({{"event", "start"},
               {"ts", QDateTime::currentDateTime().toString(Qt::ISODate)},
               {"gpu", gpu.name},
               {"luid", (gpu.luidLow || gpu.luidHigh) ? gpu.luidText() : QString()},
               {"sensor", m_guard->source()},
               {"limitC", m_limit},
               {"startC", m_lastReading.celsius},
               {"targetFps", m_options.targetFps},
               {"autoTune", autoTune},
               {"level", m_controller.level()},
               {"duration", m_duration},
               {"music", m_musicBox->isChecked()}});
    onTick();
}

void MainWindow::stopRun(const QString& reason, const QString& detail)
{
    if (!m_running) return;
    m_running = false;
    m_deadline.stop();
    m_tick.stop();
    m_thermal.stop();
    // The GPU stops here: no frame is submitted after shutdown() returns. The
    // window itself goes later — this may run inside one of its own signals.
    const RenderWindow::InputStats input =
        m_render ? m_render->inputStats() : RenderWindow::InputStats();
    const MusicPlayer::Stats audio = m_music.stats();
    m_music.stop();
    if (m_render) m_render->shutdown();
    if (m_container) m_container->deleteLater();
    m_render = nullptr;
    m_container = nullptr;

    const qint64 endNs = m_runClock.nsecsElapsed();
    const double seconds = endNs / 1e9;
    const double lockedSeconds = m_lockedSinceNs >= 0 ? (endNs - m_lockedSinceNs) / 1e9 : 0.0;
    if (reason == QLatin1String("thermal") || reason == QLatin1String("sensor-lost"))
        m_exitCode = kExitThermal;
    QString text = QString("Stopped: %1").arg(reason);
    if (!detail.isEmpty()) text += QStringLiteral(" — ") + detail;
    m_statusLabel->setText(text);
    if (reason != QLatin1String("timeout") && reason != QLatin1String("user")) {
        m_banner->setText(text);
        m_banner->show();
    }
    writeJson({{"event", "end"},
               {"reason", reason},
               {"detail", detail},
               {"seconds", seconds},
               {"level", m_controller.level()},
               {"lockedSeconds", lockedSeconds},
               {"lockedFps", lockedSeconds > 0.0 ? m_lockedFrames / lockedSeconds : 0.0},
               {"peakC", m_peakCelsius},
               {"lastC", m_lastReading.celsius},
               {"mouseMoves", input.mouseMoves},
               {"keyPresses", input.keyPresses},
               {"audioUnderruns", audio.underruns},
               {"audioError", audio.error}});

    m_startButton->setText(QStringLiteral("Start"));
    m_gpuBox->setEnabled(true);
    m_autoBox->setEnabled(true);
    m_noSensorBox->setEnabled(true);
    m_levelSlider->setEnabled(!m_autoBox->isChecked());
    if (m_options.autostart)
        QTimer::singleShot(0, qApp, [code = m_exitCode]() { QApplication::exit(code); });
}

void MainWindow::onFrame(double gpuMs)
{
    if (!m_running) return;
    ++m_frames;
    if (m_lockedSinceNs >= 0) ++m_lockedFrames;
    if (gpuMs > 0.0) {
        m_gpuMsSum += gpuMs;
        ++m_gpuMsCount;
    }
    const qint64 now = m_runClock.nsecsElapsed();
    m_controller.onFrame(now / 1e9);
    if (m_render) m_render->setLevel(m_controller.level());
    if (m_wasCalibrating && m_controller.locked()) {
        m_wasCalibrating = false;
        m_lockedSinceNs = now;
        setSliderLevel(m_controller.level());
        writeJson({{"event", "calibrated"},
                   {"t", m_runClock.nsecsElapsed() / 1e9},
                   {"level", m_controller.level()},
                   {"saturated", m_controller.saturated()}});
    }
}

void MainWindow::onTick()
{
    if (!m_running) return;
    const qint64 now = m_runClock.nsecsElapsed();
    const double span = (now - m_lastTickNs) / 1e9;
    const double fps = span > 0.0 ? m_frames / span : 0.0;
    const double gpuMs = m_gpuMsCount ? m_gpuMsSum / m_gpuMsCount : 0.0;
    const QString phase =
        m_controller.calibrating() ? QStringLiteral("calibrating") : QStringLiteral("locked");
    m_fpsLabel->setText(QString("%1 fps").arg(fps, 0, 'f', 1));
    m_gpuLabel->setText(QString("GPU %1 ms").arg(gpuMs, 0, 'f', 1));
    m_levelLabel->setText(QString("level %1 (%2)").arg(m_controller.level(), 0, 'f', 0).arg(phase));
    m_tempLabel->setText(
        QString("%1 / limit %2 °C").arg(celsiusText(m_lastReading)).arg(m_limit, 0, 'f', 0));
    m_timeLabel->setText(QString("%1 s left").arg((m_deadline.remainingTime() + 999) / 1000));
    updateInputAudio();
    const RenderWindow::InputStats input =
        m_render ? m_render->inputStats() : RenderWindow::InputStats();
    const MusicPlayer::Stats audio = m_music.stats();
    if (m_lastTickNs > 0 || m_frames > 0)
        writeJson({{"t", now / 1e9},
                   {"phase", phase},
                   {"fps", fps},
                   {"gpuMs", gpuMs},
                   {"level", m_controller.level()},
                   {"tempC", m_lastReading.celsius},
                   {"state", m_lastReading.state},
                   {"mouseMoves", input.mouseMoves},
                   {"keyPresses", input.keyPresses},
                   {"spin", input.spin},
                   {"audioUnderruns", audio.underruns},
                   {"audioBufferedMs", audio.bufferedMs}});
    m_frames = 0;
    m_gpuMsSum = 0.0;
    m_gpuMsCount = 0;
    m_lastTickNs = now;
}

void MainWindow::onThermal()
{
    if (!m_running || !m_guard) return;
    const ThermalReading r = m_guard->read();
    if (!m_guard->hasSensor()) return; // the run was allowed without one
    if (!r.ok) {
        stopRun(QStringLiteral("sensor-lost"), QStringLiteral("the temperature stopped answering"));
        return;
    }
    m_lastReading = r;
    if (r.state >= 0) {
        if (r.state >= 2) stopRun(QStringLiteral("thermal"), celsiusText(r));
        return;
    }
    m_peakCelsius = std::max(m_peakCelsius, r.celsius);
    if (r.celsius >= m_limit)
        stopRun(QStringLiteral("thermal"),
                QString("%1 reached the %2 °C limit").arg(celsiusText(r)).arg(m_limit, 0, 'f', 0));
}

void MainWindow::updateInputAudio()
{
    const RenderWindow::InputStats in =
        m_render ? m_render->inputStats() : RenderWindow::InputStats();
    m_inputLabel->setText(
        QString("Input: %1 mouse moves, %2 keys%3 · spin %4 rad/s, tilt %5°  "
                "(mouse turns, arrows spin, Space resets)")
            .arg(in.mouseMoves)
            .arg(in.keyPresses)
            .arg(in.lastKey.isEmpty() ? QString() : QString(" (last %1)").arg(in.lastKey))
            .arg(in.spin, 0, 'f', 1)
            .arg(in.tiltDegrees, 0, 'f', 0));
    const MusicPlayer::Stats a = m_music.stats();
    QString audio;
    if (!a.error.isEmpty())
        audio = QString("Music: %1").arg(a.error);
    else if (a.playing)
        audio = QString("Music: playing, %1 ms buffered, %2 underruns")
                    .arg(a.bufferedMs, 0, 'f', 0)
                    .arg(a.underruns);
    else
        audio = QStringLiteral("Music: off");
    m_audioLabel->setText(audio);
}
