/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The resolution choice, from what Settings stores to the size a launch asks
 * for. Each choice has one property that matters to the user and that only a
 * launch reveals: Auto on the native host is the host's own size brought down
 * to this screen and never blown up; "Match my screen" is this screen's size
 * and the host's display asked to take it; a custom pair that reaches a native
 * host as a height is a squeezed picture; and any of them sent as a height of
 * 0 to Sunshine is a refused launch.
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
const tablet = { width: 2000, height: 1600 };

describe('readResolutionChoice', () => {
    it('is Auto by default, for anything it does not know, and for the retired "host"', () => {
        expect(readResolutionChoice(undefined).mode).toBe('auto');
        expect(readResolutionChoice({ stream_resolution: 'native' }).mode).toBe('auto');
        expect(readResolutionChoice({ stream_resolution: 'host' }).mode).toBe('auto');
        expect(readResolutionChoice({ stream_resolution: 'fixed' }).mode).toBe('fixed');
        expect(readResolutionChoice({ stream_resolution: 'device' }).mode).toBe('device');
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
    const noFlags = {
        fitBox: false,
        allowUpscale: false,
        matchDisplay: false,
        followsScreen: false,
        fallback: null,
    };

    it('leaves a fixed rung to the ratio logic: height only', () => {
        expect(resolveStreamSize({ mode: 'fixed', height: 1440 }, { nativeHost: false })).toEqual({
            height: 1440,
            aspect: null,
            ...noFlags,
        });
    });

    // Auto on the native host: this screen is the box the host's own size
    // must fit in — the host keeps its size when it fits (1920×1080 in
    // 2000×1600), scales down at its own shape when it does not (2560×1440 →
    // 2000×1125), never up. All of that is the host's box rule; what is sent
    // is the box, and the request follows this screen.
    it('asks the native host for its own size within this screen, never upscaled', () => {
        expect(resolveStreamSize({ mode: 'auto' }, { nativeHost: true, device: tablet })).toEqual({
            height: 1600,
            aspect: '2000:1600',
            fitBox: true,
            allowUpscale: false,
            matchDisplay: false,
            followsScreen: true,
            fallback: null,
        });
        // No screen to read: the host's own size, whatever it is.
        expect(resolveStreamSize({ mode: 'auto' }, { nativeHost: true, device: null })).toEqual({
            height: 0,
            aspect: null,
            ...noFlags,
        });
    });

    it("is today's 1080p on a host that cannot say its size", () => {
        expect(resolveStreamSize({ mode: 'auto' }, { nativeHost: false, device: tablet })).toEqual({
            height: HOST_FALLBACK_HEIGHT,
            aspect: null,
            ...noFlags,
        });
    });

    // "Match my screen": this screen's size, pixel for pixel — the one choice
    // that may ask a host for more than its display has, and the one that
    // asks the native host to switch its display to that size.
    it('asks for this screen, the display switched to it, upscaling allowed', () => {
        const size = resolveStreamSize(
            { mode: 'device' },
            { nativeHost: true, device: { width: 2532, height: 1170 } },
        );
        expect(size).toEqual({
            height: 1170,
            aspect: '2532:1170',
            fitBox: true,
            allowUpscale: true,
            matchDisplay: true,
            followsScreen: true,
            fallback: { width: 2532, height: 1170 },
        });
        // The same request reaches every host: the backend turns "W:H" at
        // that height into the explicit width Sunshine needs, and GameStream's
        // optimal-settings flag lets it switch the host display to it.
        expect(
            resolveStreamSize(
                { mode: 'device' },
                { nativeHost: false, device: { width: 1920, height: 1080 } },
            ).aspect,
        ).toBe('1920:1080');
    });

    // A phone or a tablet is pinched and zoomed: Auto's box there is 1440
    // lines by any width, the same whichever way it is held — a 1440p host
    // streams as it is, a 4K one comes down to 1440, nothing goes below 1080.
    it('asks a native host for 1440 lines, any width, on a phone or a tablet', () => {
        const phoneScreen = { width: 2532, height: 1170 };
        const size = resolveStreamSize(
            { mode: 'auto' },
            { nativeHost: true, touch: true, device: phoneScreen },
        );
        expect(size.height).toBe(1440);
        expect(size.aspect).toBe('4096:1440');
        expect(size.allowUpscale).toBe(false);
        expect(size.followsScreen).toBe(false);
        // "Match my screen" on a phone falls back to that same box.
        const match = resolveStreamSize(
            { mode: 'device' },
            { nativeHost: true, touch: true, device: phoneScreen },
        );
        expect(match.aspect).toBe('2532:1170');
        expect(match.fallback).toEqual({ width: 4096, height: 1440 });
        expect(bitrateReference({ mode: 'auto' }, phoneScreen, true)).toEqual({
            height: 1440,
            aspect: '16:9',
        });
    });

    it('falls back to the fixed rung when the screen is unknown', () => {
        expect(
            resolveStreamSize({ mode: 'device', height: 720 }, { nativeHost: true, device: null }),
        ).toEqual({ height: 720, aspect: null, ...noFlags });
    });

    it('sends a custom pair as a box, even, never upscaled, nothing switched', () => {
        expect(
            resolveStreamSize(
                { mode: 'custom', customWidth: 1601, customHeight: 1201 },
                { nativeHost: true },
            ),
        ).toEqual({
            height: 1200,
            aspect: '1600:1200',
            fitBox: true,
            allowUpscale: false,
            matchDisplay: false,
            followsScreen: false,
            fallback: null,
        });
        // Out of bounds is pinned, not refused.
        const pinned = resolveStreamSize(
            { mode: 'custom', customWidth: 100, customHeight: 9000 },
            { nativeHost: false },
        );
        expect(pinned.height).toBe(CUSTOM_SIZE_MAX);
        expect(pinned.aspect).toBe(`${CUSTOM_SIZE_MIN}:${CUSTOM_SIZE_MAX}`);
    });
});

// "MoonlightWeb Virtual Display": the screen is made for this stream, so
// every choice names an exact size and the display is created at it. No
// letterbox anywhere, and a rung takes THIS screen's shape.
describe('resolveStreamSize on the virtual display', () => {
    const phoneScreen = { width: 2532, height: 1170 };
    const ask = (choice) =>
        resolveStreamSize(choice, {
            nativeHost: true,
            touch: true,
            virtualDisplay: true,
            device: phoneScreen,
        });

    it('makes the display this screen for Auto and for "Match my screen" alike', () => {
        for (const mode of ['auto', 'device']) {
            const size = ask({ mode });
            expect(size.aspect).toBe('2532:1170');
            expect(size.height).toBe(1170);
            expect(size.matchDisplay).toBe(true);
            expect(size.followsScreen).toBe(true);
            expect(size.fallback).toBeNull();
        }
    });

    it('takes a custom pair as typed, portrait and all, even and in bounds', () => {
        expect(ask({ mode: 'custom', customWidth: 1677, customHeight: 2043 }).aspect).toBe(
            '1676:2042',
        );
        // Under the floor a display mode is not usable: it is raised to it.
        expect(ask({ mode: 'custom', customWidth: 400, customHeight: 400 }).aspect).toBe('640:640');
    });

    it("gives a rung its lines at this screen's shape, shrunk when it overflows", () => {
        // 1080 lines on a 2.165 screen: 2336×1080, not 1920×1080.
        expect(ask({ mode: 'fixed', height: 1080 }).aspect).toBe('2336:1080');
        expect(ask({ mode: 'fixed', height: 720 }).aspect).toBe('1558:720');
        // 2160 lines would be 4676 wide — past what the driver and the
        // decoders take, so the whole thing comes down at its own shape.
        const tall = ask({ mode: 'fixed', height: 2160 });
        expect(tall.aspect).toBe('4096:1892');
        expect(tall.height).toBe(1892);
        // A 16:9 laptop gets exactly the rung.
        expect(
            resolveStreamSize(
                { mode: 'fixed', height: 1440 },
                { nativeHost: true, virtualDisplay: true, device: { width: 1920, height: 1080 } },
            ).aspect,
        ).toBe('2560:1440');
    });

    it('is the ordinary choice again when this screen is unknown', () => {
        const size = resolveStreamSize(
            { mode: 'auto' },
            { nativeHost: true, touch: true, virtualDisplay: true, device: null },
        );
        expect(size.aspect).toBe('4096:1440');
        expect(size.matchDisplay).toBe(false);
    });
});

describe('bitrateReference', () => {
    it('counts the pixels the choice stands for', () => {
        // Auto and "Match my screen": this screen, the most the host sends.
        expect(bitrateReference({ mode: 'auto' }, tablet)).toEqual({
            height: 1600,
            aspect: '2000:1600',
        });
        expect(bitrateReference({ mode: 'device' }, { width: 2532, height: 1170 })).toEqual({
            height: 1170,
            aspect: '2532:1170',
        });
        expect(bitrateReference({ mode: 'custom', customWidth: 2560, customHeight: 1080 })).toEqual(
            { height: 1080, aspect: '2560:1080' },
        );
        // Unknown screen: the estimate's own 1080p reference.
        expect(bitrateReference({ mode: 'auto', height: 2160 }, null)).toEqual({
            height: 1080,
            aspect: '16:9',
        });
        expect(bitrateReference({ mode: 'fixed', height: 2160 })).toEqual({
            height: 2160,
            aspect: '16:9',
        });
    });
});
