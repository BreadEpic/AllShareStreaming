/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * "Auto" frame rate: the stream runs at the rate of the screen it lands on.
 * One measurement answers for both halves — the frames per second the host is
 * asked for, and the cadence its virtual display is created at — so what is
 * worth pinning down here is the reading of that measurement: a panel that
 * runs at 164.8 Hz is 165 FPS, and a screen that could not be measured leaves
 * the choice to the caller rather than inventing a rate.
 */
import { describe, it, expect } from 'vitest';
import { autoFps, AUTO_FPS_MIN, AUTO_FPS_MAX } from '../js/util/RefreshRate.js';

describe('autoFps', () => {
    it('rounds the measured millihertz to whole frames per second', () => {
        expect(autoFps(59940)).toBe(60);
        expect(autoFps(60000)).toBe(60);
        expect(autoFps(119880)).toBe(120);
        expect(autoFps(143900)).toBe(144);
        expect(autoFps(164800)).toBe(165);
    });

    it('answers 0 when nothing was measured, so the caller keeps its default', () => {
        expect(autoFps(0)).toBe(0);
        expect(autoFps(-1)).toBe(0);
        expect(autoFps(NaN)).toBe(0);
    });

    it('stays within what an encoder and a client will carry', () => {
        expect(autoFps(1000)).toBe(AUTO_FPS_MIN);
        expect(autoFps(500000)).toBe(AUTO_FPS_MAX);
    });

    it('reads the last measurement when given no argument, 0 before any', () => {
        expect(autoFps()).toBe(0);
    });
});
