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
 * The audio jitter buffer, told what to aim for.
 *
 * The host sends one Opus frame every 5 ms and the browser decodes the track
 * itself — its NetEq buffer is the whole of what stands between the two, and
 * nothing here used to say a word about it. Left alone it sizes itself on the
 * arrival jitter it sees, and the overlay's "Audio buffer" read 129 ms on a
 * LAN with no network jitter to speak of: 200 packets a second leave the host
 * in bursts rather than in a steady stream, and NetEq reads a burst exactly as
 * it reads a congested link.
 *
 * A target puts a stake in the ground: aim for this, grow past it only if the
 * link really needs it. 60 ms is twelve Opus frames — room for a burst of a
 * dozen packets, a tenth of a second under the measured idle figure, and still
 * far above the 5 ms frame it plays out. Audio that cracks under it would mean
 * the arrival jitter is real rather than self-inflicted, which is the thing
 * this measures. Video's target is set the same way, adaptively, from
 * StreamView's stats loop (WebRtcMedia.setVideoJitterBufferTarget).
 *
 * `jitterBufferTarget` is Chromium-only; Firefox and Safari ignore it and keep
 * their own adaptive buffer, so this is a no-op there rather than a fallback.
 */

/** What the audio buffer aims for, in milliseconds. */
export const AUDIO_JITTER_TARGET_MS = 60;

/**
 * Aim every audio receiver of @p pc at @p ms milliseconds.
 *
 * @param {RTCPeerConnection} pc
 * @param {number} [ms] target in milliseconds; the default is the one above
 * @returns {number} how many receivers took it (0 on a browser without the
 *          property, which is not a failure)
 */
export function setAudioJitterBufferTarget(pc, ms = AUDIO_JITTER_TARGET_MS) {
    if (!pc || typeof pc.getReceivers !== 'function') return 0;
    const targetMs = Math.max(0, ms | 0);
    let applied = 0;
    try {
        for (const receiver of pc.getReceivers()) {
            const track = receiver && receiver.track;
            if (!track || track.kind !== 'audio') continue;
            if (receiver.jitterBufferTarget === undefined) continue;
            receiver.jitterBufferTarget = targetMs;
            applied++;
        }
    } catch (e) {
        console.warn('[Audio] jitter buffer target refused:', e.message);
    }
    return applied;
}
