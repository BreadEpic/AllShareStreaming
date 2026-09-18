/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The audio jitter buffer's target. What matters is that it reaches the audio
 * receiver and only that one — the video receiver's target is driven from the
 * measured link and must not be overwritten — and that a browser without the
 * property, or a peer connection that refuses, costs nothing.
 */
import { describe, it, expect, vi } from 'vitest';
import { AUDIO_JITTER_TARGET_MS, setAudioJitterBufferTarget } from '../js/util/AudioJitter.js';

const receiver = (kind, supported = true) => {
    const r = { track: { kind } };
    if (supported) r.jitterBufferTarget = null;
    return r;
};

describe('setAudioJitterBufferTarget', () => {
    it('aims the audio receiver and leaves the video one alone', () => {
        const audio = receiver('audio');
        const video = receiver('video');
        video.jitterBufferTarget = 180;
        const pc = { getReceivers: () => [video, audio] };
        expect(setAudioJitterBufferTarget(pc)).toBe(1);
        expect(audio.jitterBufferTarget).toBe(AUDIO_JITTER_TARGET_MS);
        expect(video.jitterBufferTarget).toBe(180);
    });

    it('takes a target of its own, whole and never negative', () => {
        const audio = receiver('audio');
        const pc = { getReceivers: () => [audio] };
        setAudioJitterBufferTarget(pc, 42.7);
        expect(audio.jitterBufferTarget).toBe(42);
        setAudioJitterBufferTarget(pc, -10);
        expect(audio.jitterBufferTarget).toBe(0);
    });

    it('is a no-op where the property does not exist — Firefox, Safari', () => {
        const audio = receiver('audio', false);
        const pc = { getReceivers: () => [audio] };
        expect(setAudioJitterBufferTarget(pc)).toBe(0);
        expect(audio.jitterBufferTarget).toBeUndefined();
    });

    it('survives no connection, no receivers and a throwing one', () => {
        expect(setAudioJitterBufferTarget(null)).toBe(0);
        expect(setAudioJitterBufferTarget({})).toBe(0);
        expect(setAudioJitterBufferTarget({ getReceivers: () => [] })).toBe(0);
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        const pc = {
            getReceivers: () => {
                throw new Error('closed');
            },
        };
        expect(setAudioJitterBufferTarget(pc)).toBe(0);
        expect(warn).toHaveBeenCalled();
        warn.mockRestore();
    });
});
