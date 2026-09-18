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
 *   - `auto`   — the default. On the native host: the host display's own size
 *                when it fits this screen, else the host's picture scaled DOWN
 *                (on the host, its shape kept) to fit this screen — a 2560×1440
 *                display on a 2000×1600 screen streams 2000×1125. The stream
 *                follows both screens: the host changing its mode, this device
 *                turning or moving to another monitor. On every other host,
 *                which cannot say its display's size: 1080p, the width from
 *                the Auto ratio (measured from Sunshine's bars).
 *   - `device` — "Match my screen": this screen's size, pixel for pixel. The
 *                native host is asked to put its display in that mode for the
 *                session (when its driver lists one) and to upscale otherwise;
 *                Sunshine and Apollo get the explicit size with GameStream's
 *                optimal-settings flag, which lets them switch the host display
 *                to it. The stream follows this screen's changes too.
 *   - `custom` — a width and a height typed in, each within
 *                [CUSTOM_SIZE_MIN, CUSTOM_SIZE_MAX]: the native host fits its
 *                display's shape inside the box, never upscaled.
 *   - `fixed`  — one of the rungs (720p … 2160p) in `stream_height`; the width
 *                follows the host's shape (Auto ratio).
 *
 * Every choice but `fixed` (and `auto` off the native host) fixes the shape
 * itself, so the aspect probe stays out of it — the size IS the request.
 */

export const RESOLUTION_MODES = ['auto', 'device', 'custom', 'fixed'];

/** Bounds of a custom width or height, in pixels. 360 is the smallest frame a
 *  hardware encoder is guaranteed to take; 4096 is DCI 4K, the widest level
 *  every browser decoder still accepts. */
export const CUSTOM_SIZE_MIN = 360;
export const CUSTOM_SIZE_MAX = 4096;

/** The height streamed under `auto` by a host that cannot say its display's
 *  size (Sunshine, Apollo, Wolf: serverinfo carries no mode). */
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
 * server's defaults), normalised: an unknown or missing mode is `auto` (so is
 * the short-lived `host` of the 0.3.1 development builds, which `auto`
 * covers), a custom size out of bounds is pinned, a missing one is 1920x1080.
 */
export function readResolutionChoice(data) {
    const d = data || {};
    const mode = RESOLUTION_MODES.includes(d.stream_resolution) ? d.stream_resolution : 'auto';
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
 * @returns {{height:number, aspect:string|null, fitBox:boolean, allowUpscale:boolean,
 *            matchDisplay:boolean, followsScreen:boolean}}
 *        `height` 0 = the host display's own size (native host only);
 *        `aspect` "W:H" fixes the width, null leaves it to the Auto ratio
 *        (probe / memory / the native host's own shape); `fitBox` tells a
 *        native host to fit its display's shape inside W×H rather than keep
 *        the height; `allowUpscale` lets it exceed its display for that;
 *        `matchDisplay` asks it to put its display in that very mode;
 *        `followsScreen` says a change of THIS screen changes the request.
 */
export function resolveStreamSize(choice, ctx) {
    const c = choice || {};
    const nativeHost = !!(ctx && ctx.nativeHost);
    const dev = ctx && ctx.device !== undefined ? ctx.device : devicePixelSize();
    const plain = { fitBox: false, allowUpscale: false, matchDisplay: false, followsScreen: false };
    const fixed = (height) => ({
        height: height > 0 ? height : HOST_FALLBACK_HEIGHT,
        aspect: null,
        ...plain,
    });
    switch (c.mode) {
        case 'auto':
            if (!nativeHost) return fixed(HOST_FALLBACK_HEIGHT);
            // The host's own size, brought down to fit this screen — its
            // shape kept, never upscaled (frameForDisplay's box rule). No
            // screen to read: the host's own size, whatever it is.
            if (!dev) return { height: 0, aspect: null, ...plain };
            return {
                height: dev.height,
                aspect: dev.width + ':' + dev.height,
                fitBox: true,
                allowUpscale: false,
                matchDisplay: false,
                followsScreen: true,
            };
        case 'device':
            if (!dev) return fixed(c.height);
            return {
                height: dev.height,
                aspect: dev.width + ':' + dev.height,
                fitBox: true,
                allowUpscale: true,
                matchDisplay: true,
                followsScreen: true,
            };
        case 'custom': {
            // Even sizes: what every encoder takes (4:2:0 chroma is half-size).
            const w = clampCustomSize(c.customWidth, 1920) & ~1;
            const h = clampCustomSize(c.customHeight, 1080) & ~1;
            return {
                height: h,
                aspect: w + ':' + h,
                fitBox: true,
                allowUpscale: false,
                matchDisplay: false,
                followsScreen: false,
            };
        }
        default:
            return fixed(c.height);
    }
}

/**
 * What the recommended-bitrate estimate should count for this choice: a
 * height and a "W:H" aspect (see util/AutoBitrate.js). `auto` counts this
 * screen — the most the native host will send under it — and 1080p 16:9 when
 * the screen is unknown, the estimate's reference.
 */
export function bitrateReference(choice, device) {
    const c = choice || {};
    const dev = device !== undefined ? device : devicePixelSize();
    switch (c.mode) {
        case 'auto':
        case 'device':
            if (dev) return { height: dev.height, aspect: dev.width + ':' + dev.height };
            return {
                height: c.mode === 'device' && c.height > 0 ? c.height : 1080,
                aspect: '16:9',
            };
        case 'custom': {
            const w = clampCustomSize(c.customWidth, 1920);
            const h = clampCustomSize(c.customHeight, 1080);
            return { height: h, aspect: w + ':' + h };
        }
        default:
            return { height: c.height > 0 ? c.height : 1080, aspect: '16:9' };
    }
}
