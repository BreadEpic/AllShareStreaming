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

#include "LoadController.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kWindowSeconds = 0.25;
// Frames already queued when the level changes were recorded at the old cost:
// the next window starts after them.
constexpr int kSkipAfterChange = 3;
} // namespace

void LoadController::reset(double level, double targetFps, double calibrateSeconds, bool autoTune)
{
    setLevel(level);
    m_target = targetFps;
    m_calibrate = calibrateSeconds;
    m_autoTune = autoTune;
    m_locked = !autoTune;
    m_saturated = false;
    m_windowStart = -1.0;
    m_windowFrames = 0;
    m_skip = kSkipAfterChange;
}

void LoadController::setLevel(double level)
{
    m_level = std::clamp(level, kMinLevel, kMaxLevel);
}

void LoadController::onFrame(double now)
{
    if (!calibrating()) return;
    if (now >= m_calibrate) {
        m_locked = true;
        return;
    }
    if (m_skip > 0) {
        --m_skip;
        m_windowStart = now;
        m_windowFrames = 0;
        return;
    }
    ++m_windowFrames;
    const double span = now - m_windowStart;
    if (span < kWindowSeconds) return;

    const double fps = m_windowFrames / span;
    // fps ~ 1 / level: the ratio is the correction, damped so one noisy window
    // cannot swing it, and bounded so a first guess far off still converges in
    // a couple of seconds without overshooting into a stall.
    const double step = std::clamp(std::pow(fps / m_target, 0.8), 0.5, 2.0);
    const double before = m_level;
    setLevel(m_level * step);
    m_saturated = (m_level >= kMaxLevel && fps > m_target);
    m_skip = (m_level != before) ? kSkipAfterChange : 0;
    m_windowStart = now;
    m_windowFrames = 0;
}
