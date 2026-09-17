/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The resolution choice, from what Settings stores to the size a launch asks
 * for. Each choice has one property that matters to the user and that only a
 * launch reveals: a device stream that is not the screen's size is not 1:1, a
 * custom pair that reaches a native host as a height is a squeezed picture,
 * and "Same as the remote PC" sent as a height of 0 to Sunshine is a refused
 * launch.
 */
import { describe, it, expect } from 'vitest';
import {
    CUSTOM_SIZE_MAX,
    CUSTOM_SIZE_MIN,
    HOST_FALLBACK_HEIGHT,
    bitrateReference,
    clampCustomSize,
    devicePixelSize,
    readResolutionChoice,
    resolveStreamSize,
} from '../js/util/StreamResolution.js';

const phone = { screen: { width: 390, height: 844 }, devicePixelRatio: 3 };
const laptop = { screen: { width: 1536, height: 864 }, devicePixelRatio: 1.25 };

describe('readResolutionChoice', () => {
    it('is the fixed rung by default, and for anything it does not know', () => {
        expect(readResolutionChoice(undefined).mode).toBe('fixed');
        expect(readResolutionChoice({ stream_resolution: 'native' }).mode).toBe('fixed');
        expect(readResolutionChoice({ stream_resolution: 'host' }).mode).toBe('host');
    });

    it('pins a stored custom pair into its bounds and fills a missing one', () => {
        const c = readResolutionChoice({
            stream_resolution: 'custom',
            stream_custom_width: 99999,
            stream_custom_height: 'abc',
        });
        expect(c.customWidth).toBe(CUSTOM_SIZE_MAX);
        expect(c.customHeight).toBe(1080);
        expect(clampCustomSize(10, 500)).toBe(CUSTOM_SIZE_MIN);
        expect(clampCustomSize('1200', 500)).toBe(1200);
    });
});

describe('devicePixelSize', () => {
    it('is the screen in physical pixels, landscape, even', () => {
        // 390×3 = 1170, 844×3 = 2532: held portrait, streamed landscape.
        expect(devicePixelSize(phone)).toEqual({ width: 2532, height: 1170 });
        // 1536×1.25 = 1920, 864×1.25 = 1080.
        expect(devicePixelSize(laptop)).toEqual({ width: 1920, height: 1080 });
    });

    it('rounds an odd result down to even', () => {
        const odd = { screen: { width: 1365, height: 767 }, devicePixelRatio: 1 };
        expect(devicePixelSize(odd)).toEqual({ width: 1364, height: 766 });
    });

    it('is null when there is no screen to read', () => {
        expect(devicePixelSize({})).toBeNull();
        expect(devicePixelSize({ screen: { width: 0, height: 0 } })).toBeNull();
    });
});

describe('resolveStreamSize', () => {
    it('leaves a fixed rung to the ratio logic: height only', () => {
        expect(resolveStreamSize({ mode: 'fixed', height: 1440 }, { nativeHost: false })).toEqual({
            height: 1440,
            aspect: null,
            fitBox: false,
            allowUpscale: false,
        });
    });

    // The one property of "Same as your device": the stream is this screen's
    // size, so full screen it is 1:1 — and it is the one choice allowed to ask
    // a host for more than its display has.
    it('asks for this screen, pixel for pixel, upscaling allowed', () => {
        const size = resolveStreamSize(
            { mode: 'device' },
            { nativeHost: true, device: { width: 2532, height: 1170 } },
        );
        expect(size).toEqual({
            height: 1170,
            aspect: '2532:1170',
            fitBox: true,
            allowUpscale: true,
        });
        // The same request reaches every host: it is the backend that turns
        // "W:H" at that height into the explicit width Sunshine needs.
        expect(
            resolveStreamSize(
                { mode: 'device' },
                { nativeHost: false, device: { width: 1920, height: 1080 } },
            ).aspect,
        ).toBe('1920:1080');
    });

    it('falls back to the fixed rung when the screen is unknown', () => {
        expect(
            resolveStreamSize({ mode: 'device', height: 720 }, { nativeHost: true, device: null }),
        ).toEqual({ height: 720, aspect: null, fitBox: false, allowUpscale: false });
    });

    // "Same as the remote PC": only the native host can answer it, with a
    // height of 0. Sent to Sunshine, 0 would be a refused launch — so it gets
    // 1080p, and the choice itself is kept for the day the host is native.
    it('is the host display for a native host, 1080p for any other', () => {
        expect(resolveStreamSize({ mode: 'host', height: 1440 }, { nativeHost: true })).toEqual({
            height: 0,
            aspect: null,
            fitBox: false,
            allowUpscale: false,
        });
        expect(resolveStreamSize({ mode: 'host', height: 1440 }, { nativeHost: false })).toEqual({
            height: HOST_FALLBACK_HEIGHT,
            aspect: null,
            fitBox: false,
            allowUpscale: false,
        });
    });

    it('sends a custom pair as a box, even, never upscaled', () => {
        expect(
            resolveStreamSize(
                { mode: 'custom', customWidth: 1601, customHeight: 1201 },
                { nativeHost: true },
            ),
        ).toEqual({ height: 1200, aspect: '1600:1200', fitBox: true, allowUpscale: false });
        // Out of bounds is pinned, not refused.
        expect(
            resolveStreamSize(
                { mode: 'custom', customWidth: 100, customHeight: 9000 },
                { nativeHost: false },
            ),
        ).toEqual({
            height: CUSTOM_SIZE_MAX,
            aspect: `${CUSTOM_SIZE_MIN}:${CUSTOM_SIZE_MAX}`,
            fitBox: true,
            allowUpscale: false,
        });
    });
});

describe('bitrateReference', () => {
    it('counts the pixels the choice stands for', () => {
        expect(bitrateReference({ mode: 'device' }, { width: 2532, height: 1170 })).toEqual({
            height: 1170,
            aspect: '2532:1170',
        });
        expect(bitrateReference({ mode: 'custom', customWidth: 2560, customHeight: 1080 })).toEqual(
            { height: 1080, aspect: '2560:1080' },
        );
        // Unknown until launch: the estimate's own 1080p reference.
        expect(bitrateReference({ mode: 'host', height: 2160 })).toEqual({
            height: 1080,
            aspect: '16:9',
        });
        expect(bitrateReference({ mode: 'fixed', height: 2160 })).toEqual({
            height: 2160,
            aspect: '16:9',
        });
    });
});
