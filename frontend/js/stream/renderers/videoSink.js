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
 * The writable VideoFrame sink that drives a <video> element, under whichever
 * name this browser gives it: Chromium ships MediaStreamTrackGenerator, WebKit
 * the standardized VideoTrackGenerator. Returns the constructor, or null when
 * neither exists — and then this browser has no native-HDR presentation path.
 *
 * Its own module rather than BrowserDetect's: createRenderer is imported by the
 * decode worker too, and BrowserDetect touches document/window while loading.
 *
 * @returns {?(new (init?: { kind: 'audio' | 'video' }) => { writable: WritableStream, track?: MediaStreamTrack })}
 */
export function videoSinkCtor() {
    try {
        if (typeof MediaStreamTrackGenerator !== 'undefined') return MediaStreamTrackGenerator;
        if (typeof VideoTrackGenerator !== 'undefined') return VideoTrackGenerator;
    } catch (e) {}
    return null;
}
