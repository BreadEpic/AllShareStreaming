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
 *                display on a 2000×1600 laptop streams 2000×1125. On a phone or
 *                a tablet the box is not the screen but 1440 lines by any width
 *                (MOBILE_AUTO_BOX): the picture is pinched and zoomed there,
 *                and a stream reduced to the screen's 1170 lines blurs the
 *                moment it is; so a host of up to 1440 lines streams as it is
 *                and a taller one comes down to 1440, never below 1080 that
 *                way. The stream follows both screens: the host changing its
 *                mode, a computer's window moving to another monitor. On every
 *                other host, which cannot say its display's size: 1080p, the
 *                width from the Auto ratio (measured from Sunshine's bars).
 *   - `device` — "Match my screen": this screen's size, pixel for pixel. The
 *                native host is asked to put its display in that mode for the
 *                session; when its driver lists no such mode the request falls
 *                back to Auto (`fallback`), which is what the host then gets.
 *                Sunshine and Apollo get the explicit size with GameStream's
 *                optimal-settings flag, which lets them switch the host display
 *                to it. The stream follows this screen's changes.
 *   - `custom` — a width and a height typed in, each within
 *                [CUSTOM_SIZE_MIN, CUSTOM_SIZE_MAX]: the native host fits its
 *                display's shape inside the box, never upscaled.
 *   - `fixed`  — one of the rungs (720p … 2160p) in `stream_height`; the width
 *                follows the host's shape (Auto ratio).
 *
 * Every choice but `fixed` (and `auto` off the native host) fixes the shape
 * itself, so the aspect probe stays out of it — the size IS the request.
 *
 * ── "MoonlightWeb Virtual Display" ─────────────────────────────────────────
 *
 * A display that exists only for this stream is not something to fit into: it
 * is made to measure, so on that one card every choice names an exact size and
 * the display is created at it (`virtualDisplay` in the context). Auto and
 * "Match my screen" both mean this screen, pixel for pixel, which is a
 * fullscreen with no bars anywhere. Custom is the pair as typed, portrait
 * included. A rung is that many lines at THIS screen's shape — 1080p on a
 * 2532×1170 phone is 2336×1080, not 1920×1080 — because the shape of a
 * display nobody sees has no reason to be anyone else's. Every one of them is
 * held inside [MODE_SIZE_MIN, MODE_SIZE_MAX], the driver's and the decoders'
 * limits, shrunk at its own shape when it does not fit.
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

/** Auto's box on a phone or a tablet: 1440 lines, any width. A host of up to
 *  1440 lines streams as it is (the pinch zoom stays sharp), a 4K one comes
 *  down to 1440 (a phone decoding 4K heats up for pixels it cannot show),
 *  and nothing is ever reduced below 1080 that way. */
export const MOBILE_AUTO_BOX = { width: CUSTOM_SIZE_MAX, height: 1440 };

/** Bounds of a display mode made on demand — what the virtual display driver
 *  will take, and what the decoder on the other side will. The floor is a
 *  desktop still usable; the ceiling is DCI 4K. Mirrored in C++ by
 *  VirtualDisplay::kModeMin / kModeMax, which pin the request again. */
export const MODE_SIZE_MIN = 640;
export const MODE_SIZE_MAX = CUSTOM_SIZE_MAX;

/**
 * A size a display can be made at: even, inside the mode bounds, and brought
 * down at its own shape rather than squeezed when it is too big — a 2160p rung
 * at a phone's 2.16 shape asks for 4676×2160 and gets 4096×1892.
 */
export function fitModeBounds(width, height) {
    let w = Math.round(width);
    let h = Math.round(height);
    if (!(w > 0) || !(h > 0)) return null;
    const over = Math.max(w / MODE_SIZE_MAX, h / MODE_SIZE_MAX);
    if (over > 1) {
        w = Math.round(w / over);
        h = Math.round(h / over);
    }
    w = Math.max(MODE_SIZE_MIN, w) & ~1;
    h = Math.max(MODE_SIZE_MIN, h) & ~1;
    return { width: w, height: h };
}

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

/** Auto's box for a native host: this screen on a computer, MOBILE_AUTO_BOX
 *  on a phone or a tablet, null when neither is known. */
export function autoBox(device, touch) {
    if (touch) return MOBILE_AUTO_BOX;
    return device || null;
}

/**
 * The size to make "MoonlightWeb Virtual Display" at for this choice, or null
 * when this screen is unknown and there is nothing to measure against.
 *
 * Auto and "Match my screen" are the same thing here — this screen, pixel for
 * pixel — because a display made to order has no size of its own to reconcile
 * with. Custom is the typed pair, portrait included. A rung is its lines at
 * this screen's shape.
 */
export function virtualDisplaySize(choice, device) {
    const c = choice || {};
    if (c.mode === 'custom')
        return fitModeBounds(
            clampCustomSize(c.customWidth, 1920),
            clampCustomSize(c.customHeight, 1080),
        );
    if (!device || !(device.width > 0) || !(device.height > 0)) return null;
    if (c.mode === 'auto' || c.mode === 'device') return fitModeBounds(device.width, device.height);
    const lines = c.height > 0 ? c.height : HOST_FALLBACK_HEIGHT;
    return fitModeBounds((lines * device.width) / device.height, lines);
}

/**
 * The size a launch asks for, from the choice and the host.
 *
 * @param {{mode:string, height:number, customWidth:number, customHeight:number}} choice
 *        the Settings choice; `height` is the `fixed` rung
 * @param {{nativeHost:boolean, touch?:boolean, virtualDisplay?:boolean,
 *          device?: {width:number,height:number}|null}} ctx
 *        whether the host is MoonlightWeb's own native host, whether this is a
 *        phone or a tablet, whether the app launched is "MoonlightWeb Virtual
 *        Display" (every choice then names an exact size, made to order — see
 *        virtualDisplaySize), and this screen (devicePixelSize() when omitted)
 * @returns {{height:number, aspect:string|null, fitBox:boolean, allowUpscale:boolean,
 *            matchDisplay:boolean, followsScreen:boolean,
 *            fallback:{width:number,height:number}|null}}
 *        `height` 0 = the host display's own size (native host only);
 *        `aspect` "W:H" fixes the width, null leaves it to the Auto ratio
 *        (probe / memory / the native host's own shape); `fitBox` tells a
 *        native host to fit its display's shape inside W×H rather than keep
 *        the height; `allowUpscale` lets it exceed its display for that;
 *        `matchDisplay` asks it to put its display in that very mode, and
 *        `fallback` is the box it fits instead when it cannot (Auto's);
 *        `followsScreen` says a change of THIS screen changes the request.
 */
export function resolveStreamSize(choice, ctx) {
    const c = choice || {};
    const nativeHost = !!(ctx && ctx.nativeHost);
    const touch = !!(ctx && ctx.touch);
    const dev = ctx && ctx.device !== undefined ? ctx.device : devicePixelSize();
    if (nativeHost && ctx && ctx.virtualDisplay) {
        const made = virtualDisplaySize(c, dev);
        if (made)
            return {
                height: made.height,
                aspect: made.width + ':' + made.height,
                fitBox: true,
                // The display IS that size, so there is nothing to scale; the
                // flag only says the frame may reach the size asked for.
                allowUpscale: true,
                matchDisplay: true,
                // Turned, or moved to another monitor: the display is remade.
                followsScreen: true,
                fallback: null,
            };
    }
    const plain = {
        fitBox: false,
        allowUpscale: false,
        matchDisplay: false,
        followsScreen: false,
        fallback: null,
    };
    const fixed = (height) => ({
        height: height > 0 ? height : HOST_FALLBACK_HEIGHT,
        aspect: null,
        ...plain,
    });
    switch (c.mode) {
        case 'auto': {
            if (!nativeHost) return fixed(HOST_FALLBACK_HEIGHT);
            // The host's own size, brought down to fit the box — its shape
            // kept, never upscaled (frameForDisplay's box rule). No screen to
            // read: the host's own size, whatever it is.
            const box = autoBox(dev, touch);
            if (!box) return { height: 0, aspect: null, ...plain };
            return {
                height: box.height,
                aspect: box.width + ':' + box.height,
                fitBox: true,
                allowUpscale: false,
                matchDisplay: false,
                // A phone's box is the same whichever way it is held.
                followsScreen: !touch,
                fallback: null,
            };
        }
        case 'device':
            if (!dev) return fixed(c.height);
            return {
                height: dev.height,
                aspect: dev.width + ':' + dev.height,
                fitBox: true,
                allowUpscale: true,
                matchDisplay: true,
                followsScreen: true,
                fallback: autoBox(dev, touch),
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
                fallback: null,
            };
        }
        default:
            return fixed(c.height);
    }
}

/**
 * What the recommended-bitrate estimate should count for this choice: a
 * height and a "W:H" aspect (see util/AutoBitrate.js). `auto` counts this
 * screen — the most the native host will send under it — 1440p 16:9 on a
 * phone or a tablet, and 1080p 16:9 when the screen is unknown, the
 * estimate's reference.
 */
export function bitrateReference(choice, device, touch) {
    const c = choice || {};
    const dev = device !== undefined ? device : devicePixelSize();
    switch (c.mode) {
        case 'auto':
            if (touch) return { height: MOBILE_AUTO_BOX.height, aspect: '16:9' };
            if (dev) return { height: dev.height, aspect: dev.width + ':' + dev.height };
            return { height: 1080, aspect: '16:9' };
        case 'device':
            if (dev) return { height: dev.height, aspect: dev.width + ':' + dev.height };
            return { height: c.height > 0 ? c.height : 1080, aspect: '16:9' };
        case 'custom': {
            const w = clampCustomSize(c.customWidth, 1920);
            const h = clampCustomSize(c.customHeight, 1080);
            return { height: h, aspect: w + ':' + h };
        }
        default:
            return { height: c.height > 0 ? c.height : 1080, aspect: '16:9' };
    }
}
