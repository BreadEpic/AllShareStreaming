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

/// Finds the load level that renders at the target frame rate, then holds it.
///
/// The level is one number that scales the scene's cost about linearly (see
/// RenderWindow::setLevel), so the frame rate goes as its inverse: every
/// quarter second the level is multiplied by (fps / target), damped. Once the
/// calibration time is over the level is FROZEN: were it still tuned while a
/// stream runs, the tool would give back to the encoder exactly the GPU time
/// the test is meant to take from it.
class LoadController
{
public:
    static constexpr double kMinLevel = 1.0;
    static constexpr double kMaxLevel = 400000.0;

    /// `autoTune` false: the level stays where it is set, no calibration.
    void reset(double level, double targetFps, double calibrateSeconds, bool autoTune);
    /// One frame completed at `nowSeconds` since the run started.
    void onFrame(double nowSeconds);

    double level() const { return m_level; }
    void setLevel(double level);
    bool calibrating() const { return m_autoTune && !m_locked; }
    bool locked() const { return m_locked; }
    /// The level hit the ceiling while still above the target: the GPU is
    /// faster than the scene can load it.
    bool saturated() const { return m_saturated; }

private:
    double m_level = 40.0;
    double m_target = 45.0;
    double m_calibrate = 8.0;
    bool m_autoTune = true;
    bool m_locked = false;
    bool m_saturated = false;
    double m_windowStart = -1.0;
    int m_windowFrames = 0;
    int m_skip = 0;
};
