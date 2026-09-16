/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * StatsGraph — the history strip's observable behaviour: what a sequence of
 * ticks turns into, which lanes it earns, and how the scale moves. The drawing
 * needs a canvas and is not exercised here.
 */
import { describe, it, expect } from 'vitest';
import { StatsGraph } from '../js/ui/StatsGraph.js';

/** Lane labels are the card's own; the test only needs them to be distinct. */
const name = (k) => k;

/** A graph that plots from the first tick — the warm-up has its own test. */
const fresh = (opts = {}) => new StatsGraph({ warmupMs: 0, ...opts });

/** One tick's worth of a healthy stream, at t = 1000 + i * 500. */
function tick(g, i, over = {}) {
    g.sample(1000 + i * 500, {
        latencyMs: 35,
        latencyP99Ms: 48,
        fpsIn: 60,
        fpsOut: 59,
        bytes: i * 1.25e6, // 20 Mbps at 500ms per tick
        netLost: 0,
        frameSpan: i * 30,
        dropStale: 0,
        decoded: i * 30,
        events: 0,
        ...over,
    });
}

describe('StatsGraph', () => {
    it('draws no lane before it has been fed', () => {
        const g = fresh();
        expect(g.lanes(name)).toEqual([]);
        expect(g.height(name)).toBe(0);
    });

    it('keeps the values it is given, newest last', () => {
        const g = fresh();
        tick(g, 0);
        tick(g, 1, { latencyMs: 90 });
        expect(g.latency.count).toBe(2);
        expect(g.latency.at(0)).toBe(35);
        expect(g.latency.last()).toBe(90);
    });

    it('ignores a repaint that lands between two ticks', () => {
        const g = fresh();
        tick(g, 0);
        // _setLatencyDetail repaints the card off-tick so the panel follows the
        // pointer: that must not push a zero-length sample.
        g.sample(1100, { latencyMs: 35, fpsIn: 60 });
        expect(g.latency.count).toBe(1);
    });

    it('differences the byte counter instead of plotting the session average', () => {
        const g = fresh();
        tick(g, 0);
        tick(g, 1);
        // 1.25 MB in 500 ms = 20 Mbps, whatever the session has averaged.
        expect(g.mbps.last()).toBeCloseTo(20, 3);
    });

    it('falls back to a reported bitrate when no byte counter is given', () => {
        const g = fresh();
        g.sample(1000, { mbps: 12, bytes: NaN });
        g.sample(1500, { mbps: 12, bytes: NaN });
        expect(g.mbps.last()).toBe(12);
    });

    it('rates losses over the last seconds of frames, not the session', () => {
        const g = fresh();
        tick(g, 0);
        tick(g, 1);
        // 3 lost out of the 60 frames of the two ticks in the rate window.
        tick(g, 2, { netLost: 3, dropStale: 6 });
        expect(g.lossNet.last()).toBeCloseTo(5, 6);
        expect(g.lossJit.last()).toBeCloseTo(10, 6);
        // The loss ages out of the window instead of staying in the average.
        for (let i = 3; i < 16; i++) tick(g, i, { netLost: 3, dropStale: 6 });
        expect(g.lossNet.last()).toBe(0);
    });

    it('still rates losses when a tick carries no new frame', () => {
        // An idle desktop at 2 fps: most 500ms ticks have nothing in them, and
        // a per-tick rate would be a dotted line of gaps.
        const g = fresh();
        let span = 0;
        for (let i = 0; i < 12; i++) {
            if (i % 2 === 0) span += 1;
            tick(g, i, { frameSpan: span, decoded: span, netLost: 0, dropStale: 0 });
        }
        expect(Number.isFinite(g.lossNet.last())).toBe(true);
        expect(g.lossNet.last()).toBe(0);
    });

    it('measures through the warm-up but plots none of it', () => {
        const g = new StatsGraph({ warmupMs: 3000 });
        // The opening seconds: late first frames, a cold decoder, counters that
        // jump on their own. Kept out of the window they would otherwise flatten.
        for (let i = 0; i < 6; i++) tick(g, i, { latencyMs: 900 });
        expect(g.latency.count).toBe(0);
        tick(g, 6, { latencyMs: 35 });
        expect(g.latency.count).toBe(1);
        expect(g.latency.last()).toBe(35);
        // The counters were tracked through it, so the first bitrate kept is one
        // tick's worth and not the whole warm-up's.
        expect(g.mbps.last()).toBeCloseTo(20, 3);
    });

    it('sets the axis on the bulk of the window, not on one spike', () => {
        const g = fresh();
        for (let i = 0; i < 40; i++) tick(g, i, { latencyMs: 35, latencyP99Ms: 48 });
        tick(g, 40, { latencyMs: 1800, latencyP99Ms: 1900 });
        for (let i = 41; i < 60; i++) tick(g, i, { latencyMs: 35, latencyP99Ms: 48 });
        // max() would put the ceiling at 2000 and squash a minute of readings
        // onto the baseline; the 95th percentile keeps the curve readable and
        // the spike is marked at the top edge instead.
        expect(g.latency.percentile(0.95)).toBeLessThan(100);
        expect(g.latency.max()).toBe(1800);
    });

    it('draws its reference line at the mean of what it holds, gaps excluded', () => {
        // The lane's reference is a value the user can read, not a ceiling in
        // a corner: it has to be the true average of the plotted samples.
        const g = fresh();
        tick(g, 0, { latencyMs: 20 });
        tick(g, 1, { latencyMs: NaN });
        tick(g, 2, { latencyMs: 40 });
        expect(g.latency.mean()).toBe(30);
        expect(fresh().latency.mean()).toBeNaN();
    });

    it('marks the tick where something had to be recovered', () => {
        const g = fresh();
        tick(g, 0);
        tick(g, 1);
        tick(g, 2, { events: 1 });
        tick(g, 3, { events: 1 });
        expect(g.events.at(1)).toBe(0);
        expect(g.events.at(2)).toBe(1);
        expect(g.events.at(3)).toBe(0);
    });

    it('drops a lane nothing ever reported', () => {
        const g = fresh();
        // The native media track: no frame counters, so no loss lane.
        for (let i = 0; i < 4; i++) {
            g.sample(1000 + i * 500, {
                latencyMs: 30,
                fpsIn: 60,
                mbps: 15,
                netLost: NaN,
                frameSpan: NaN,
                dropStale: NaN,
                decoded: NaN,
            });
        }
        expect(g.lanes(name).map((l) => l.key)).toEqual(['latency', 'fps', 'bitrate']);
    });

    it('forgets samples older than its window', () => {
        const g = fresh({ windowMs: 2000, periodMs: 500 });
        for (let i = 0; i < 20; i++) tick(g, i, { latencyMs: i });
        expect(g.latency.count).toBe(5);
        expect(g.latency.last()).toBe(19);
        expect(g.latency.at(0)).toBe(15);
    });

    it('raises the ceiling at once and lowers it only after a quiet run', () => {
        const g = fresh();
        const steps = [20, 50, 100, 200];
        expect(g.ceiling('l', steps, 12)).toBe(20);
        // A rise is honoured on the spot — the plot must hold what it is given.
        expect(g.ceiling('l', steps, 140)).toBe(200);
        // Coming back down takes four quiet ticks, so a stream that spikes every
        // few seconds is not drawn on a rescaling axis.
        expect(g.ceiling('l', steps, 30)).toBe(200);
        expect(g.ceiling('l', steps, 30)).toBe(200);
        expect(g.ceiling('l', steps, 30)).toBe(200);
        expect(g.ceiling('l', steps, 30)).toBe(50);
    });

    it('gives each lane its own scale', () => {
        const g = fresh();
        expect(g.ceiling('a', [10, 100], 90)).toBe(100);
        expect(g.ceiling('b', [10, 100], 5)).toBe(10);
    });

    it('grows the strip with the lanes that have data', () => {
        const g = fresh();
        tick(g, 0);
        tick(g, 1);
        const four = g.height(name);
        expect(g.lanes(name).length).toBe(4);
        const g2 = fresh();
        g2.sample(1000, { latencyMs: 30 });
        g2.sample(1500, { latencyMs: 30 });
        expect(g2.lanes(name).length).toBe(1);
        expect(g2.height(name)).toBeLessThan(four);
        expect(g2.height(name)).toBeGreaterThan(0);
    });
});
