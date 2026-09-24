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
 * How the picture fills the stream area when the two shapes differ — a
 * browser window that is not in fullscreen is shorter than the host's screen,
 * so the picture gets bars on its sides.
 *
 *   - 'fit':     whole picture, bars around it (object-fit: contain)
 *   - 'stretch': whole picture, no bars, slightly squeezed (object-fit: fill)
 *   - 'zoom':    no bars, the edges that overflow are cut (object-fit: cover)
 *
 * Kept with the other streaming settings (`picture_fill`), so the Settings
 * page and the in-stream shortcut change the same value.
 */

export const PICTURE_FILLS = ['fit', 'stretch', 'zoom'];

const STORAGE_KEY = 'mw-streaming-settings';

/** A stored value, or 'fit' for anything unknown. */
export function normalizePictureFill(value) {
    return PICTURE_FILLS.includes(value) ? value : 'fit';
}

/** The mode the in-stream shortcut switches to after `value`. */
export function nextPictureFill(value) {
    const i = PICTURE_FILLS.indexOf(normalizePictureFill(value));
    return PICTURE_FILLS[(i + 1) % PICTURE_FILLS.length];
}

/** The saved mode, 'fit' when there is none. */
export function loadPictureFill() {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        const settings = raw ? JSON.parse(raw) : null;
        return normalizePictureFill(settings && settings.picture_fill);
    } catch (e) {
        return 'fit';
    }
}

/** Save the mode without touching any other streaming setting. */
export function savePictureFill(value) {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        const parsed = raw ? JSON.parse(raw) : null;
        const settings = parsed && typeof parsed === 'object' ? parsed : {};
        settings.picture_fill = normalizePictureFill(value);
        localStorage.setItem(STORAGE_KEY, JSON.stringify(settings));
    } catch (e) {
        console.warn('[MW] Could not save the picture fill mode:', e);
    }
}

/**
 * Where a `iw`×`ih` picture lands inside the `rect` box for a fill mode — the
 * same geometry the CSS object-fit draws, so input positions map onto what the
 * viewer sees.
 */
export function pictureRect(rect, iw, ih, fill) {
    if (!iw || !ih) return rect;
    if (fill === 'stretch') {
        return { left: rect.left, top: rect.top, width: rect.width, height: rect.height };
    }
    const scale =
        fill === 'zoom'
            ? Math.max(rect.width / iw, rect.height / ih)
            : Math.min(rect.width / iw, rect.height / ih);
    const w = iw * scale,
        h = ih * scale;
    return {
        left: rect.left + (rect.width - w) / 2,
        top: rect.top + (rect.height - h) / 2,
        width: w,
        height: h,
    };
}
