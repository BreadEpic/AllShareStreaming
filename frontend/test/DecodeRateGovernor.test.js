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
 * DecodeRateGovernor — when the client asks the host for fewer frames, and when
 * it asks for them back.
 */
import { describe, it, expect } from 'vitest';
import { DecodeRateGovernor } from '../js/stream/DecodeRateGovernor.js';

/** Feed the same observation once a second; returns every cap change. */
function feed(gov, obs, seconds, startMs) {
    const changes = [];
    for (let s = 0; s <= seconds; s++) {
        const r = gov.update({ ...obs, now: startMs + s * 1000 });
        if (r !== null) changes.push(r);
    }
    return changes;
}

/** The measured case: 116 arriving, 105 decoded, ten frames standing. */
const STANDING = { decodeQueue: 10, decodedFps: 105 };
const CLEAR = { decodeQueue: 0, decodedFps: 97 };

describe('DecodeRateGovernor', () => {
    it('leaves a healthy stream alone', () => {
        const gov = new DecodeRateGovernor(120);
        expect(feed(gov, { decodeQueue: 0.2, decodedFps: 120 }, 60, 1000)).toEqual([]);
        expect(gov.cap).toBe(0);
    });

    it('asks for less than the decoder delivers once the queue has stood', () => {
        const gov = new DecodeRateGovernor(120);
        expect(feed(gov, STANDING, 3, 1000)).toEqual([97]); // 105 × 0.93
        expect(gov.cap).toBe(97);
    });

    it('lets a burst drain by itself', () => {
        const gov = new DecodeRateGovernor(120);
        feed(gov, STANDING, 2, 1000);
        feed(gov, CLEAR, 1, 4000);
        expect(feed(gov, STANDING, 2, 6000)).toEqual([]);
        expect(gov.cap).toBe(0);
    });

    it('does not read the window that still holds the old rate', () => {
        const gov = new DecodeRateGovernor(120);
        feed(gov, STANDING, 3, 1000);
        // Two seconds later the averages are still those of 120fps.
        expect(gov.update({ ...STANDING, now: 6000 })).toBe(null);
    });

    it('steps down again when the first cap was not enough', () => {
        const gov = new DecodeRateGovernor(120);
        feed(gov, STANDING, 3, 1000);
        expect(feed(gov, { decodeQueue: 8, decodedFps: 90 }, 4, 10000)).toEqual([83]);
    });

    it('never goes under half the setting', () => {
        const gov = new DecodeRateGovernor(120);
        let t = 1000;
        for (let i = 0; i < 10; i++) {
            feed(gov, { decodeQueue: 12, decodedFps: 20 }, 4, t);
            t += 10000;
        }
        expect(gov.cap).toBe(60);
    });

    it('gives a tenth of the setting back after twenty clear seconds, then the rest', () => {
        const gov = new DecodeRateGovernor(120);
        feed(gov, STANDING, 3, 1000);
        expect(feed(gov, CLEAR, 19, 10000)).toEqual([]);
        expect(feed(gov, CLEAR, 2, 30000)).toEqual([109]);
        // 109 + 12 reaches the setting: the cap is lifted, not set to 120.
        expect(feed(gov, CLEAR, 25, 40000)).toEqual([0]);
        expect(gov.cap).toBe(0);
    });

    it('waits twice as long after a raise that did not hold', () => {
        const gov = new DecodeRateGovernor(120);
        feed(gov, STANDING, 3, 1000);
        feed(gov, CLEAR, 22, 10000); // raised to 109 at 30s
        expect(gov.cap).toBe(109);
        feed(gov, STANDING, 4, 36000); // and down again within the regret delay
        expect(gov.cap).toBe(97);
        expect(feed(gov, CLEAR, 35, 50000)).toEqual([]);
        expect(feed(gov, CLEAR, 10, 86000)).toEqual([109]);
    });

    it('stays out of a stream whose rate it does not know', () => {
        const gov = new DecodeRateGovernor(0);
        expect(feed(gov, STANDING, 10, 1000)).toEqual([]);
    });
});
