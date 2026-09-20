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
 * DecodeQueuePolicy — when a keyframe behind a deep queue is worth a flush.
 */
import { describe, it, expect } from 'vitest';
import { shouldFlushAtKeyframe, FLUSH_COOLDOWN_MS } from '../js/stream/DecodeQueuePolicy.js';

const NEVER = Infinity;

describe('shouldFlushAtKeyframe', () => {
    it('flushes the queue it was written for', () => {
        expect(
            shouldFlushAtKeyframe({ queued: 348, arrivalMs: 16.7, sinceLastFlushMs: NEVER }),
        ).toBe(true);
    });

    it('decodes through a few frames at 120fps rather than rebuild the decoder', () => {
        // The measured loop: 8 to 33 frames waiting, 8.5ms apart.
        for (const queued of [8, 13, 28]) {
            expect(shouldFlushAtKeyframe({ queued, arrivalMs: 8.5, sinceLastFlushMs: NEVER })).toBe(
                false,
            );
        }
    });

    it('judges the same depth by what it holds in time', () => {
        const at = (arrivalMs) =>
            shouldFlushAtKeyframe({ queued: 20, arrivalMs, sinceLastFlushMs: NEVER });
        expect(at(8.3)).toBe(false); // 166ms
        expect(at(16.7)).toBe(true); // 334ms
    });

    it('does not repeat a flush that has just been tried', () => {
        const obs = { queued: 60, arrivalMs: 8.3 };
        expect(shouldFlushAtKeyframe({ ...obs, sinceLastFlushMs: 400 })).toBe(false);
        expect(shouldFlushAtKeyframe({ ...obs, sinceLastFlushMs: FLUSH_COOLDOWN_MS })).toBe(true);
    });

    it('assumes 60fps before an arrival has been measured', () => {
        expect(shouldFlushAtKeyframe({ queued: 14, arrivalMs: 0, sinceLastFlushMs: NEVER })).toBe(
            false,
        );
        expect(shouldFlushAtKeyframe({ queued: 16, arrivalMs: 0, sinceLastFlushMs: NEVER })).toBe(
            true,
        );
    });
});
