/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The recommended bitrate. One property carries the whole rule: it is the
 * count of pixels per second the frame really holds, against the 1080p60 SDR
 * reference — and half of it on a phone or a tablet, where the screen is
 * hand-sized, the decoder is the weak link and the link is mobile data.
 */
import { describe, it, expect } from 'vitest';
import { aspectToNumber, computeAutoBitrate } from '../js/util/AutoBitrate.js';

describe('aspectToNumber', () => {
    it('reads "W:H" and falls back to 16:9', () => {
        expect(aspectToNumber('2560:1080')).toBeCloseTo(2560 / 1080);
        expect(aspectToNumber('auto')).toBeCloseTo(16 / 9);
        expect(aspectToNumber('16:0')).toBeCloseTo(16 / 9);
        expect(aspectToNumber(undefined)).toBeCloseTo(16 / 9);
    });
});

describe('computeAutoBitrate', () => {
    it('is 20 Mbps at the 1080p60 SDR reference and scales with the pixels', () => {
        expect(computeAutoBitrate(1080, 60, '16:9', false, false)).toBe(20);
        expect(computeAutoBitrate(0, 60, '16:9', false, false)).toBe(20);
        expect(computeAutoBitrate(1440, 60, '16:9', false, false)).toBe(36);
        expect(computeAutoBitrate(1080, 120, '16:9', false, false)).toBe(40);
        expect(computeAutoBitrate(1080, 60, '16:9', false, true)).toBe(30);
    });

    it('is halved on a phone or a tablet, and only there', () => {
        expect(computeAutoBitrate(1080, 60, '16:9', false, false, true)).toBe(10);
        expect(computeAutoBitrate(1440, 60, '16:9', false, false, true)).toBe(18);
        expect(computeAutoBitrate(2160, 60, '16:9', false, false, true)).toBe(40);
        // A desktop is untouched — it usually streams over the LAN.
        expect(computeAutoBitrate(1440, 60, '16:9', false, false, false)).toBe(36);
    });

    it('never leaves [1, 150] Mbps', () => {
        expect(computeAutoBitrate(360, 30, '16:9', false, false, true)).toBe(1);
        expect(computeAutoBitrate(4320, 240, '32:9', true, true)).toBe(150);
    });
});
