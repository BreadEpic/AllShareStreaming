/*
 * MoonlightWeb — native capture & encoding engine.
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

#include "core/FrameCadence.h"
#include "native_test_framework.h"

#include <vector>

using mw::native::FrameCadence;

namespace {

/// Drive a cadence with presents at a fixed refresh rate for @p seconds, the
/// way the capture loop does: an admitted present is encoded on the spot, a
/// skipped one is never encoded. Returns the encode times, in µs.
struct Sim
{
    std::vector<int64_t> encodedAt;
    int presents = 0;
    int64_t skipped = 0;
};

/// Presents at @p presentHz — which need not be the display's nominal rate —
/// with an optional alternating jitter of ±@p jitterUs.
Sim simulateOn(FrameCadence cadence, double presentHz, double seconds, int64_t jitterUs = 0,
               int64_t startUs = 0)
{
    Sim sim;
    const double presentStep = 1e6 / presentHz;
    for (double t = 0; t < seconds * 1e6; t += presentStep) {
        const int64_t presentUs =
            startUs + static_cast<int64_t>(t) + ((sim.presents % 2) ? jitterUs : -jitterUs);
        sim.presents++;
        if (cadence.admit(presentUs)) sim.encodedAt.push_back(presentUs);
    }
    sim.skipped = cadence.skipped();
    return sim;
}

Sim simulate(int fps, int displayHz, double seconds, int64_t startUs = 0)
{
    return simulateOn(FrameCadence(fps, displayHz), displayHz, seconds, 0, startUs);
}

void gaps(const Sim& sim, int64_t& minGap, int64_t& maxGap)
{
    minGap = 1 << 30;
    maxGap = 0;
    for (size_t i = 1; i < sim.encodedAt.size(); ++i) {
        const int64_t gap = sim.encodedAt[i] - sim.encodedAt[i - 1];
        if (gap < minGap) minGap = gap;
        if (gap > maxGap) maxGap = gap;
    }
}

} // namespace

void run_frame_cadence_tests()
{
    SECTION("FrameCadence — zero means every present");
    {
        FrameCadence off(0);
        CHECK(!off.enabled());
        for (int i = 0; i < 10; ++i)
            CHECK(off.admit(i * 6060));
        CHECK_EQ(off.skipped(), int64_t(0));

        FrameCadence negative(-5, 165);
        CHECK(!negative.enabled());
        CHECK(negative.admit(0));
    }

    SECTION("FrameCadence — 165 Hz presents into a 60 fps stream");
    {
        // Sub-microsecond drift is not what is being tested: a 60 fps interval
        // is 16666 µs and the presents step 6060.6.
        const Sim sim = simulate(60, 165, 10.0);
        // 165 presents/s in, 60 frames/s out. The grid advances by its interval
        // on every admitted present rather than restarting on it, so the rate
        // holds — within a frame either way for the edges of the run.
        CHECK(sim.presents >= 1649 && sim.presents <= 1651);
        CHECK(sim.encodedAt.size() >= 599 && sim.encodedAt.size() <= 601);
        CHECK_EQ(sim.skipped, int64_t(sim.presents) - int64_t(sim.encodedAt.size()));

        // Nothing is ever held, so the emission is as regular as the presents
        // allow: every gap between two encodes is within one present period of
        // the stream interval.
        int64_t minGap, maxGap;
        gaps(sim, minGap, maxGap);
        CHECK(minGap > 16666 - 6061);
        CHECK(maxGap < 16666 + 6061);
    }

    SECTION("FrameCadence — 165 Hz presents into a 120 fps stream");
    {
        // The present period (6.06 ms) is more than half the interval (8.33):
        // an early present and a late one can both land inside one wall-clock
        // interval, but each consumes one tick of the grid, so the rate is
        // still the setting's.
        const Sim sim = simulate(120, 165, 10.0);
        CHECK(sim.encodedAt.size() >= 1199 && sim.encodedAt.size() <= 1201);
    }

    SECTION("FrameCadence — 100 Hz presents into a 60 fps stream");
    {
        const Sim sim = simulate(60, 100, 10.0);
        CHECK(sim.encodedAt.size() >= 599 && sim.encodedAt.size() <= 601);
        int64_t minGap, maxGap;
        gaps(sim, minGap, maxGap);
        CHECK(minGap > 16666 - 10001);
        CHECK(maxGap < 16666 + 10001);
    }

    SECTION("FrameCadence — a display slower than the stream is never throttled");
    {
        // 60 Hz presents into a 120 fps stream: every present is past due when
        // it arrives, nothing is skipped.
        const Sim sim = simulate(120, 60, 5.0);
        CHECK_EQ(sim.encodedAt.size(), size_t(sim.presents));
        CHECK_EQ(sim.skipped, int64_t(0));
    }

    SECTION("FrameCadence — the first present at or after the tick, the others skipped");
    {
        FrameCadence c(60, 165); // 16666 µs interval, slack 4166
        CHECK(c.admit(0));       // first frame goes straight through
        CHECK_EQ(c.nextDueUs(), int64_t(16666));
        CHECK(!c.admit(6000));
        CHECK(!c.admit(12000)); // 12000 + 4166 < 16666: still too early
        CHECK_EQ(c.skipped(), int64_t(2));
        CHECK(c.admit(18000)); // encoded the moment it arrives
        // The grid advanced by one interval from where it was, not from the
        // slightly late present.
        CHECK_EQ(c.nextDueUs(), int64_t(33332));
    }

    SECTION("FrameCadence — a present within the slack of the tick goes through now");
    {
        FrameCadence c(60, 165);
        CHECK(c.admit(0));
        // Due at 16666; a present at 13000 is within a quarter interval of it.
        CHECK(c.admit(13000));
        CHECK_EQ(c.skipped(), int64_t(0));
        // …and the grid kept its phase rather than restarting on it.
        CHECK_EQ(c.nextDueUs(), int64_t(33332));
    }

    SECTION("FrameCadence — after a still screen the first change is immediate and re-anchors");
    {
        FrameCadence c(60, 165);
        CHECK(c.admit(0));
        // Two seconds of nothing, then a present: encoded now, and the grid
        // restarts on it rather than on a tick nobody had been keeping.
        CHECK(c.admit(2000000));
        CHECK_EQ(c.nextDueUs(), int64_t(2016666));
    }

    SECTION("FrameCadence — a missing present re-anchors, a merely late one keeps the grid");
    {
        // Late by 7 ms after a skipped present. On a 165 Hz display a present
        // was missing (the period is 6.06 ms): the screen's cadence is out of
        // phase with the grid, lock onto it.
        FrameCadence fast(60, 165);
        CHECK_EQ(fast.reanchorUs(), int64_t(6060));
        CHECK(fast.admit(0));
        CHECK(!fast.admit(9666));
        CHECK(fast.admit(23666));
        CHECK_EQ(fast.nextDueUs(), int64_t(23666 + 16666));

        // Without a display rate the threshold is half an interval: 7 ms late
        // is ordinary lateness, the grid keeps its phase.
        FrameCadence blind(60);
        CHECK_EQ(blind.reanchorUs(), int64_t(8333));
        CHECK(blind.admit(0));
        CHECK(!blind.admit(9666));
        CHECK(blind.admit(23666));
        CHECK_EQ(blind.nextDueUs(), int64_t(33332));
    }

    SECTION("FrameCadence — a game at the stream's rate locks onto the grid");
    {
        // 60 fps presents, out of phase with a 60 fps grid on a 165 Hz display:
        // the first is too early and skipped, the next one is late by more
        // than a present period — a present was missing — and re-anchors the
        // grid; from then on every present goes through. One drop, not a beat.
        FrameCadence c(60, 165);
        CHECK(c.admit(0));
        int encoded = 0, skipped = 0;
        for (int i = 0; i < 300; ++i) {
            // Presents 10 ms before the grid's ticks, drifting nowhere.
            if (c.admit(10000 + i * 16666))
                encoded++;
            else
                skipped++;
        }
        CHECK_EQ(encoded, 299);
        CHECK_EQ(skipped, 1);
        CHECK_EQ(c.nextDueUs(), int64_t(10000 + 300 * 16666));
    }

    SECTION("FrameCadence — a ceiling never skips a display that keeps to its rate");
    {
        const FrameCadence c = FrameCadence::ceiling(1000000000LL / 60, 60);
        CHECK(c.enabled());
        CHECK(c.isCeiling());
        CHECK(!FrameCadence(60, 165).isCeiling());
        // A fiftieth faster than the stream: 16339 µs for 16666.
        CHECK_EQ(c.intervalUs(), int64_t(16339));

        // A 60 Hz stream on a 60 Hz display, whether the panel runs a little
        // slow (59.94, 59.95 here on DualRTX), exactly, or a little fast:
        // every present goes through, for minutes.
        for (double hz : {59.94, 60.0, 60.06}) {
            const Sim sim = simulateOn(c, hz, 120.0);
            CHECK_EQ(sim.skipped, int64_t(0));
            CHECK_EQ(sim.encodedAt.size(), size_t(sim.presents));
        }
        // Presents a millisecond and a half either side of their refresh —
        // far more than Desktop Duplication's timestamps stray — still all go.
        const Sim jittered = simulateOn(c, 60.0, 60.0, 1500);
        CHECK_EQ(jittered.skipped, int64_t(0));

        // A present the loop picks up late, then the next one on time: 3 ms
        // apart. Both go — the one skipped would have been a picture the
        // viewer waited a whole frame for.
        FrameCadence pair = FrameCadence::ceiling(1000000000LL / 120, 120);
        CHECK_EQ(pair.slackUs(), pair.intervalUs());
        for (int i = 0; i < 10; ++i)
            CHECK(pair.admit(i * 16666));
        CHECK(pair.admit(10 * 16666 + 5000));
        CHECK(pair.admit(10 * 16666 + 8000));
        CHECK_EQ(pair.skipped(), int64_t(0));
    }

    SECTION("FrameCadence — a ceiling holds presents beyond the refresh to the stream's rate");
    {
        // What Desktop Duplication reported on a 59.95 Hz screen while a
        // browser on it ticked with a 120 Hz primary: 73 to 104 presents a
        // second. The stream is set to 60: no more than 61.2 go out.
        for (double hz : {73.0, 104.0, 120.0, 300.0}) {
            const Sim sim =
                simulateOn(FrameCadence::ceiling(1000000000LL / 60, 60), hz, 10.0, 0, 5000);
            CHECK(sim.encodedAt.size() >= 590 && sim.encodedAt.size() <= 615);
        }
        // A stream set ABOVE the display's rate: the display's own refreshes
        // all go through, a runaway source stops at the setting.
        const FrameCadence at120 = FrameCadence::ceiling(1000000000LL / 120, 60);
        CHECK_EQ(simulateOn(at120, 60.0, 10.0).skipped, int64_t(0));
        const Sim runaway = simulateOn(at120, 300.0, 10.0);
        CHECK(runaway.encodedAt.size() >= 1190 && runaway.encodedAt.size() <= 1230);
    }

    SECTION("FrameCadence — no interval, no ceiling");
    {
        const FrameCadence c = FrameCadence::ceiling(0, 60);
        CHECK(!c.enabled());
        CHECK(!c.isCeiling());
    }
}
