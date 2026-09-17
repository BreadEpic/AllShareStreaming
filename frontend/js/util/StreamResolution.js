/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

/**
 * The streamed resolution, from the choice in Settings to the size a launch
 * asks for.
 *
 * Four ways to choose it (`stream_resolution`):
 *   - `fixed`  — one of the rungs (720p … 2160p) in `stream_height`; the width
 *                follows the host's shape (Auto ratio: measured, or stated by
 *                the native host). Today's behaviour, and the default.
 *   - `device` — this device's own screen, pixel for pixel: the stream is the
 *                screen's size, so a full-screen picture is shown 1:1. The one
 *                choice that may ask a host for MORE than its display has.
 *   - `host`   — the host display's own size. Only the native host can answer
 *                it; every other host streams 1080p under this choice, and the
 *                choice is kept, since the next session may well be native.
 *   - `custom` — a width and a height typed in, each within
 *                [CUSTOM_SIZE_MIN, CUSTOM_SIZE_MAX].
 *
 * Every choice but `fixed` fixes the shape too, so the ratio is Auto under all
 * of them and the aspect probe stays out of it — the size IS the request.
 */

export const RESOLUTION_MODES = ['fixed', 'device', 'host', 'custom'];

/** Bounds of a custom width or height, in pixels. 360 is the smallest frame a
 *  hardware encoder is guaranteed to take; 4096 is DCI 4K, the widest level
 *  every browser decoder still accepts. */
export const CUSTOM_SIZE_MIN = 360;
export const CUSTOM_SIZE_MAX = 4096;

/** The height streamed under "Same as the remote PC" by a host that cannot say
 *  its display's size (Sunshine, Apollo, Wolf: serverinfo carries no mode). */
export const HOST_FALLBACK_HEIGHT = 1080;

/** The rungs of `fixed` — also the congestion ladder's steps. */
export const FIXED_HEIGHTS = [720, 1080, 1440, 2160];

/** Pin a typed size into its bounds; anything unreadable becomes `fallback`. */
export function clampCustomSize(value, fallback) {
    const n = typeof value === 'number' ? value : parseInt(value, 10);
    if (!Number.isFinite(n)) return fallback;
    return Math.min(CUSTOM_SIZE_MAX, Math.max(CUSTOM_SIZE_MIN, Math.round(n)));
}

/**
 * Read the resolution choice out of a settings object (localStorage or the
 * server's defaults), normalised: an unknown mode is `fixed`, a custom size
 * out of bounds is pinned, a missing one is 1920x1080.
 */
export function readResolutionChoice(data) {
    const d = data || {};
    const mode = RESOLUTION_MODES.includes(d.stream_resolution) ? d.stream_resolution : 'fixed';
    return {
        mode,
        customWidth: clampCustomSize(d.stream_custom_width, 1920),
        customHeight: clampCustomSize(d.stream_custom_height, 1080),
    };
}

/**
 * This device's screen in physical pixels, landscape (the stream is a desktop,
 * shown landscape whatever way the phone is held), even in both dimensions.
 * @param {{screen?: {width:number,height:number}, devicePixelRatio?: number}} [win]
 * @returns {{width:number, height:number}|null} null when the screen is unknown
 */
export function devicePixelSize(win) {
    const w = win || (typeof window !== 'undefined' ? window : null);
    const s = w && w.screen;
    if (!s || !(s.width > 0) || !(s.height > 0)) return null;
    const dpr = w.devicePixelRatio > 0 ? w.devicePixelRatio : 1;
    const a = Math.round(s.width * dpr) & ~1;
    const b = Math.round(s.height * dpr) & ~1;
    if (!(a > 0) || !(b > 0)) return null;
    return { width: Math.max(a, b), height: Math.min(a, b) };
}

/**
 * The size a launch asks for, from the choice and the host.
 *
 * @param {{mode:string, height:number, customWidth:number, customHeight:number}} choice
 *        the Settings choice; `height` is the `fixed` rung
 * @param {{nativeHost:boolean, device?: {width:number,height:number}|null}} ctx
 *        whether the host is MoonlightWeb's own native host, and this screen
 *        (devicePixelSize() when omitted)
 * @returns {{height:number, aspect:string|null, fitBox:boolean, allowUpscale:boolean}}
 *        `height` 0 = the host display's own size (native host only);
 *        `aspect` "W:H" fixes the width, null leaves it to the Auto ratio
 *        (probe / memory / the native host's own shape); `fitBox` tells a
 *        native host to fit its display's shape inside W×H rather than keep
 *        the height; `allowUpscale` lets it exceed its display for that.
 */
export function resolveStreamSize(choice, ctx) {
    const c = choice || {};
    const nativeHost = !!(ctx && ctx.nativeHost);
    const fixed = () => ({
        height: c.height > 0 ? c.height : HOST_FALLBACK_HEIGHT,
        aspect: null,
        fitBox: false,
        allowUpscale: false,
    });
    switch (c.mode) {
        case 'device': {
            const dev = ctx && ctx.device !== undefined ? ctx.device : devicePixelSize();
            if (!dev) return fixed();
            return {
                height: dev.height,
                aspect: dev.width + ':' + dev.height,
                fitBox: true,
                allowUpscale: true,
            };
        }
        case 'host':
            return nativeHost
                ? { height: 0, aspect: null, fitBox: false, allowUpscale: false }
                : {
                      height: HOST_FALLBACK_HEIGHT,
                      aspect: null,
                      fitBox: false,
                      allowUpscale: false,
                  };
        case 'custom': {
            // Even sizes: what every encoder takes (4:2:0 chroma is half-size).
            const w = clampCustomSize(c.customWidth, 1920) & ~1;
            const h = clampCustomSize(c.customHeight, 1080) & ~1;
            return { height: h, aspect: w + ':' + h, fitBox: true, allowUpscale: false };
        }
        default:
            return fixed();
    }
}

/**
 * What the recommended-bitrate estimate should count for this choice: a
 * height and a "W:H" aspect (see util/AutoBitrate.js). `host` is unknown until
 * launch and counts as 1080p 16:9, the estimate's reference.
 */
export function bitrateReference(choice, device) {
    const c = choice || {};
    switch (c.mode) {
        case 'device': {
            const dev = device !== undefined ? device : devicePixelSize();
            if (dev) return { height: dev.height, aspect: dev.width + ':' + dev.height };
            return { height: c.height > 0 ? c.height : 1080, aspect: '16:9' };
        }
        case 'host':
            return { height: 1080, aspect: '16:9' };
        case 'custom': {
            const w = clampCustomSize(c.customWidth, 1920);
            const h = clampCustomSize(c.customHeight, 1080);
            return { height: h, aspect: w + ':' + h };
        }
        default:
            return { height: c.height > 0 ? c.height : 1080, aspect: '16:9' };
    }
}
