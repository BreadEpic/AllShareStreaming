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
 * StatsGraph — the last minute of the stream, under the stats card's numbers.
 *
 * The overlay's rows say what the stream is doing RIGHT NOW. A number that
 * jumps tells you something moved; it cannot tell you whether the latency is
 * climbing steadily (the link is degrading) or spiked once and came back (a
 * single hitch), and by the time the user looks, the moment is already gone.
 * This class keeps the last 60 seconds of every value the card shows and draws
 * them as four stacked lanes.
 *
 * Four lanes, never one plot: ms, fps, Mbps and % have nothing to do with each
 * other on a shared axis, and a graph with two y-scales is a graph that lies.
 * Two curves share a lane ONLY when they share a unit, and there it is the
 * point — the gap between decoded and presented fps IS the jitter drop.
 *
 *   LATENCY   total (solid) + the composed p99 (hairline)
 *   FRAMERATE decoded in (solid) + presented out (hairline)
 *   BITRATE   filled area — a magnitude reads as an area
 *   LOSS      network loss (solid, red) + jitter drops (hairline, neutral)
 *
 * Plus one vertical tick across all four lanes per recovery / stall / ride-out,
 * because "it froze about ten seconds ago" and "the decoder was rebuilt HERE,
 * while the bitrate collapsed" are two very different bug reports.
 *
 * Sampling is deliberately independent of display: StreamView pushes a sample
 * on every 500 ms overlay tick whether or not the breakdown is open, so opening
 * it right after a hitch shows the minute that CONTAINS the hitch instead of an
 * empty box that fills in once the incident has passed. The cost of that is
 * seven floats per tick.
 *
 * Drawing is pure canvas, on the same 500 ms tick — no rAF, no per-frame work,
 * nothing on the path a frame takes to the screen.
 *
 * The buffers and the scale logic hold no DOM, so they are testable on their
 * own; draw() is the only part that needs a context.
 */

/** Seconds of history kept and drawn. */
export const GRAPH_WINDOW_MS = 60000;

/** Nominal spacing between samples — the overlay's own refresh period. */
const SAMPLE_PERIOD_MS = 500;

/** One lane: header line + plot. */
const LANE_LABEL_H = 12;
const LANE_PLOT_H = 22;
const LANE_H = LANE_LABEL_H + LANE_PLOT_H;
const LANE_GAP = 5;
/** Room under the last lane for the "−60s … 0s" time ruler. */
const FOOTER_H = 10;
/** Separator above the whole strip, matching the card's other rules. */
const HEAD_H = 5;

/**
 * Lane colors. Amber is not a free choice: it is the color the latency VALUE
 * already wears two rows above, and a graph of that value in another color
 * would read as another measurement. The other three were picked around it and
 * checked for colorblind separation against the card's glass (ΔE ≥ 11 in
 * deutan/protan/tritan, ≥ 20 in normal vision). They sit above the usual dark
 * lightness band on purpose: this card is translucent over arbitrary game
 * content, sometimes a snow level, and a chart-on-solid-dark step disappears
 * there.
 */
const C_LATENCY = '#f0c040';
const C_FPS = '#5ad2f0';
const C_BITRATE = '#b58bff';
const C_LOSS = '#ff6b6b';
/** The "arrived late", not "never arrived", series: neutral, not a fifth hue. */
const C_LOSS_SOFT = 'rgba(255,255,255,0.45)';

const INK_LABEL = 'rgba(255,255,255,0.32)';
const INK_UNIT = 'rgba(255,255,255,0.22)';
const INK_AXIS = 'rgba(255,255,255,0.10)';
const INK_CEIL = 'rgba(255,255,255,0.26)';

const FONT_STACK = "'SF Mono', 'Cascadia Code', 'Consolas', monospace";

/**
 * Ceilings each lane is allowed to snap to. A free "max of the window" scale
 * redraws the curve at a new height on every tick — the eye reads that as the
 * stream moving when only the axis did.
 */
const STEPS_MS = [20, 50, 100, 150, 200, 300, 500, 750, 1000, 2000];
const STEPS_FPS = [30, 60, 90, 120, 144, 240, 360];
const STEPS_MBPS = [2, 5, 10, 20, 30, 50, 75, 100, 150, 200, 500];
const STEPS_PCT = [1, 2, 5, 10, 25, 50, 100];

/** A fixed-size ring of numbers; NaN marks "no sample". */
class Ring {
    /** @param {number} capacity */
    constructor(capacity) {
        this._buf = new Float32Array(capacity).fill(NaN);
        this._head = 0; // next write
        this._count = 0;
    }

    /** @param {number} v */
    push(v) {
        this._buf[this._head] = v;
        this._head = (this._head + 1) % this._buf.length;
        if (this._count < this._buf.length) this._count++;
    }

    get count() {
        return this._count;
    }

    /** Oldest-first value at index i (0 … count-1). */
    at(i) {
        const len = this._buf.length;
        return this._buf[(this._head - this._count + i + len * 2) % len];
    }

    /** Largest finite value, or NaN when the ring holds none. */
    max() {
        let m = NaN;
        for (let i = 0; i < this._count; i++) {
            const v = this.at(i);
            if (!Number.isFinite(v)) continue;
            if (!(m >= v)) m = v;
        }
        return m;
    }

    /** True when at least one sample is a real number. */
    hasData() {
        return Number.isFinite(this.max());
    }

    /** Most recent finite value, or NaN. */
    last() {
        for (let i = this._count - 1; i >= 0; i--) {
            const v = this.at(i);
            if (Number.isFinite(v)) return v;
        }
        return NaN;
    }
}

/**
 * @typedef {Object} StatsSample
 * @property {number} [latencyMs]    End-to-end total, as the card shows it.
 * @property {number} [latencyP99Ms] Per-stage p99s composed — the worst case.
 * @property {number} [fpsIn]        Decoded frames per second.
 * @property {number} [fpsOut]       Presented frames per second.
 * @property {number} [mbps]         Instantaneous bitrate, when the caller has one.
 * @property {number} [bytes]        Cumulative bytes received — differenced here.
 * @property {number} [netLost]      Cumulative frames lost on the wire.
 * @property {number} [frameSpan]    Cumulative frames the host said it sent.
 * @property {number} [dropStale]    Cumulative frames dropped at the render stage.
 * @property {number} [decoded]      Cumulative frames decoded.
 * @property {number} [events]       Cumulative recoveries + stalls + ride-outs.
 */

export class StatsGraph {
    constructor({ windowMs = GRAPH_WINDOW_MS, periodMs = SAMPLE_PERIOD_MS } = {}) {
        this._periodMs = periodMs;
        const cap = Math.ceil(windowMs / periodMs) + 1;
        this._cap = cap;
        this.latency = new Ring(cap);
        this.latencyP99 = new Ring(cap);
        this.fpsIn = new Ring(cap);
        this.fpsOut = new Ring(cap);
        this.mbps = new Ring(cap);
        this.lossNet = new Ring(cap);
        this.lossJit = new Ring(cap);
        /** 1 where something had to be recovered, 0 otherwise. */
        this.events = new Ring(cap);
        /** Previous cumulative counters, for the per-tick deltas. */
        this._prev = null;
        this._lastAt = -Infinity;
        /** Per-lane ceiling state: { step, low } — `low` counts quiet ticks. */
        this._scales = {};
    }

    /** Seconds of history the ring can hold. */
    get windowMs() {
        return (this._cap - 1) * this._periodMs;
    }

    /**
     * Record one tick. Cumulative counters are differenced against the previous
     * call, so the curves show what happened DURING the tick — the card's own
     * bitrate and loss figures are session averages, which on a graph would be
     * a flat line that never says when anything went wrong.
     *
     * A call that lands too soon after the last one is ignored: _setLatencyDetail
     * repaints the card off-tick so the panel tracks the pointer, and that must
     * not push a zero-length sample. Nothing is lost — the counters are
     * cumulative, so the next real tick covers the gap.
     *
     * @param {number} now performance.now()
     * @param {StatsSample} s
     */
    sample(now, s) {
        if (now - this._lastAt < this._periodMs * 0.5) return;
        const prev = this._prev;
        const dt = prev ? (now - this._lastAt) / 1000 : 0;
        this._lastAt = now;

        const num = (v) => (Number.isFinite(v) ? v : NaN);
        /** Delta of a cumulative counter; NaN until there is a previous one. */
        const delta = (key) => {
            if (!prev || !Number.isFinite(s[key]) || !Number.isFinite(prev[key])) return NaN;
            return Math.max(0, s[key] - prev[key]);
        };

        this.latency.push(num(s.latencyMs));
        this.latencyP99.push(num(s.latencyP99Ms));
        this.fpsIn.push(num(s.fpsIn));
        this.fpsOut.push(num(s.fpsOut));

        // Bitrate: prefer a differenced byte count (the truth for this tick);
        // fall back to whatever instantaneous figure the caller has, which is
        // what the native media track reports through getStats.
        let mbps = NaN;
        const dBytes = delta('bytes');
        if (Number.isFinite(dBytes) && dt > 0) mbps = (dBytes * 8) / dt / 1e6;
        else if (Number.isFinite(s.mbps)) mbps = s.mbps;
        this.mbps.push(mbps);

        // Loss rates over the tick's own frames, not the session's.
        const dSpan = delta('frameSpan');
        const dLost = delta('netLost');
        this.lossNet.push(dSpan > 0 && Number.isFinite(dLost) ? (dLost / dSpan) * 100 : NaN);
        const dDec = delta('decoded');
        const dStale = delta('dropStale');
        this.lossJit.push(dDec > 0 && Number.isFinite(dStale) ? (dStale / dDec) * 100 : NaN);

        const dEvents = delta('events');
        this.events.push(Number.isFinite(dEvents) && dEvents > 0 ? 1 : 0);

        this._prev = {
            bytes: num(s.bytes),
            netLost: num(s.netLost),
            frameSpan: num(s.frameSpan),
            dropStale: num(s.dropStale),
            decoded: num(s.decoded),
            events: num(s.events),
        };
    }

    /**
     * The ceiling for a lane: the smallest allowed step above the window's max,
     * held on the way down. Shrinking the instant the peak scrolls out makes a
     * calm stream look like it is breathing; four quiet ticks below 55% of the
     * current step is a real change of regime, not a gap between spikes.
     *
     * @param {string} key lane identity (its own hysteresis state)
     * @param {number[]} steps
     * @param {number} max
     */
    ceiling(key, steps, max) {
        const st = this._scales[key] || (this._scales[key] = { step: steps[0], low: 0 });
        if (!Number.isFinite(max)) return st.step;
        if (max > st.step) {
            st.step = steps.find((v) => v >= max) || steps[steps.length - 1];
            st.low = 0;
        } else if (max < st.step * 0.55) {
            if (++st.low >= 4) {
                st.step = steps.find((v) => v >= max) || steps[0];
                st.low = 0;
            }
        } else {
            st.low = 0;
        }
        return st.step;
    }

    /**
     * The lanes that have something to show, in fixed order. A lane with no
     * sample at all is dropped rather than drawn empty: on the native media
     * track the browser owns the pipeline and there are no frame counters, and
     * a permanently flat "LOSS 0%" there would be a claim we cannot make.
     *
     * @param {(k: string) => string} name lane-label lookup
     */
    lanes(name) {
        /** @type {Array<Object>} */
        const all = [
            {
                key: 'latency',
                label: name('latency'),
                unit: 'ms',
                steps: STEPS_MS,
                series: [
                    { ring: this.latency, color: C_LATENCY, width: 1.4 },
                    { ring: this.latencyP99, color: C_LATENCY, width: 1, alpha: 0.38 },
                ],
                value: () => fmtPair(this.latency.last(), this.latencyP99.last(), 1),
            },
            {
                key: 'fps',
                label: name('framerate'),
                unit: '',
                steps: STEPS_FPS,
                series: [
                    { ring: this.fpsIn, color: C_FPS, width: 1.4 },
                    { ring: this.fpsOut, color: C_FPS, width: 1, alpha: 0.4 },
                ],
                value: () => fmtPair(this.fpsIn.last(), this.fpsOut.last(), 0),
            },
            {
                key: 'bitrate',
                label: name('bitrate'),
                unit: 'Mbps',
                steps: STEPS_MBPS,
                series: [{ ring: this.mbps, color: C_BITRATE, width: 1.4, fill: true }],
                value: () => fmtOne(this.mbps.last(), 1),
            },
            {
                key: 'loss',
                label: name('loss'),
                unit: '%',
                steps: STEPS_PCT,
                series: [
                    { ring: this.lossNet, color: C_LOSS, width: 1.4 },
                    { ring: this.lossJit, color: C_LOSS_SOFT, width: 1 },
                ],
                value: () => fmtPair(this.lossNet.last(), this.lossJit.last(), 2),
            },
        ];
        return all.filter((lane) => lane.series.some((s) => s.ring.hasData()));
    }

    /**
     * Height the strip needs, in CSS pixels, for the lanes that have data.
     * @param {(k: string) => string} name
     */
    height(name) {
        const n = this.lanes(name).length;
        if (!n) return 0;
        return HEAD_H + n * LANE_H + (n - 1) * LANE_GAP + FOOTER_H;
    }

    /**
     * Draw the strip. The context is expected to be in CSS pixels already (the
     * caller applies the device-pixel-ratio transform).
     *
     * @param {CanvasRenderingContext2D} ctx
     * @param {number} w
     * @param {(k: string) => string} name
     */
    draw(ctx, w, name) {
        const lanes = this.lanes(name);
        if (!lanes.length || w <= 0) return;
        const h = this.height(name);
        ctx.clearRect(0, 0, w, h);

        // The rule that separates the strip from the numbers above it, drawn
        // rather than borrowed from CSS so the canvas box stays the bitmap.
        ctx.fillStyle = 'rgba(255,255,255,0.07)';
        ctx.fillRect(0, 0, w, 1);

        let y = HEAD_H;
        for (const lane of lanes) {
            this._drawLane(ctx, lane, y, w);
            y += LANE_H + LANE_GAP;
        }

        // Event ticks LAST and across every lane: their whole value is that you
        // can see what the bitrate and the fps were doing at that instant.
        const top = HEAD_H;
        const bottom = y - LANE_GAP;
        const n = this.events.count;
        if (n > 1) {
            ctx.fillStyle = 'rgba(255,107,107,0.4)';
            for (let i = 0; i < n; i++) {
                if (!this.events.at(i)) continue;
                const x = Math.round(xAt(i, n, w)) + 0.5;
                ctx.fillRect(x, top, 1, bottom - top);
            }
        }

        // Time ruler: the window is only obvious once it is written down.
        ctx.font = `8px ${FONT_STACK}`;
        ctx.fillStyle = INK_CEIL;
        ctx.textBaseline = 'alphabetic';
        ctx.textAlign = 'left';
        ctx.fillText('−' + Math.round(this.windowMs / 1000) + 's', 0, h - 2);
        ctx.textAlign = 'right';
        ctx.fillText('0s', w, h - 2);
    }

    /** @param {CanvasRenderingContext2D} ctx */
    _drawLane(ctx, lane, y, w) {
        const plotTop = y + LANE_LABEL_H;
        const plotBottom = plotTop + LANE_PLOT_H;

        // Header: the lane's name and its current value, in the card's own
        // voice — uppercase muted label left, tabular value right. The value is
        // repeated here because a curve without its number is a shape, and the
        // user reading the graph should never have to look back up the card.
        ctx.textBaseline = 'alphabetic';
        ctx.font = `9px ${FONT_STACK}`;
        ctx.textAlign = 'left';
        ctx.fillStyle = INK_LABEL;
        const label = lane.label.toUpperCase();
        ctx.fillText(label, 0, y + 9);
        if (lane.unit) {
            ctx.fillStyle = INK_UNIT;
            ctx.fillText(' ' + lane.unit, ctx.measureText(label).width, y + 9);
        }
        ctx.textAlign = 'right';
        ctx.fillStyle = lane.series[0].color;
        ctx.font = `10px ${FONT_STACK}`;
        ctx.fillText(lane.value(), w, y + 9);

        // Baseline at zero, and the ceiling as a dotted rule with its figure —
        // a curve with no scale is decoration.
        const max = Math.max(
            ...lane.series.map((s) => {
                const m = s.ring.max();
                return Number.isFinite(m) ? m : 0;
            }),
        );
        const ceil = this.ceiling(lane.key, lane.steps, max);
        ctx.fillStyle = INK_AXIS;
        ctx.fillRect(0, plotBottom, w, 1);
        ctx.save();
        ctx.setLineDash([1, 3]);
        ctx.strokeStyle = 'rgba(255,255,255,0.07)';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(0, plotTop + 0.5);
        ctx.lineTo(w, plotTop + 0.5);
        ctx.stroke();
        ctx.restore();
        ctx.font = `8px ${FONT_STACK}`;
        ctx.fillStyle = INK_CEIL;
        ctx.textAlign = 'right';
        ctx.fillText(String(ceil), w, plotTop + 8);

        for (const s of lane.series) this._drawSeries(ctx, s, plotTop, plotBottom, w, ceil);
    }

    /** @param {CanvasRenderingContext2D} ctx */
    _drawSeries(ctx, s, top, bottom, w, ceil) {
        const ring = s.ring;
        const n = ring.count;
        if (n < 2) return;
        const height = bottom - top;
        const yOf = (v) => bottom - Math.max(0, Math.min(1, v / ceil)) * height;

        ctx.save();
        ctx.globalAlpha = s.alpha || 1;
        ctx.strokeStyle = s.color;
        ctx.lineWidth = s.width;
        ctx.lineJoin = 'round';
        ctx.lineCap = 'round';

        // One path per run of finite samples: a gap in the data is a gap in the
        // line, never a straight segment across it pretending to be a reading.
        let open = false;
        let lastX = 0;
        let lastY = 0;
        const fillPts = [];
        ctx.beginPath();
        for (let i = 0; i < n; i++) {
            const v = ring.at(i);
            if (!Number.isFinite(v)) {
                open = false;
                continue;
            }
            const x = xAt(i, n, w);
            const yy = yOf(v);
            if (!open) {
                ctx.moveTo(x, yy);
                open = true;
            } else {
                ctx.lineTo(x, yy);
            }
            fillPts.push([x, yy]);
            lastX = x;
            lastY = yy;
        }
        if (s.fill && fillPts.length > 1) {
            const grad = ctx.createLinearGradient(0, top, 0, bottom);
            grad.addColorStop(0, hexA(s.color, 0.28));
            grad.addColorStop(1, hexA(s.color, 0.02));
            ctx.save();
            ctx.beginPath();
            ctx.moveTo(fillPts[0][0], bottom);
            for (const [x, yy] of fillPts) ctx.lineTo(x, yy);
            ctx.lineTo(fillPts[fillPts.length - 1][0], bottom);
            ctx.closePath();
            ctx.fillStyle = grad;
            ctx.fill();
            ctx.restore();
            ctx.beginPath();
            let started = false;
            for (const [x, yy] of fillPts) {
                if (!started) {
                    ctx.moveTo(x, yy);
                    started = true;
                } else ctx.lineTo(x, yy);
            }
        }
        ctx.stroke();

        // The now-end of the line gets a dot: on a 22px lane it is the
        // difference between "the value is there" and "the value stopped".
        if (open) {
            ctx.beginPath();
            ctx.arc(lastX, lastY, s.width, 0, Math.PI * 2);
            ctx.fillStyle = s.color;
            ctx.fill();
        }
        ctx.restore();
    }
}

/** Sample i of n mapped across the plot, oldest at the left edge. */
function xAt(i, n, w) {
    return n <= 1 ? w : (i / (n - 1)) * w;
}

/** "12.3 / 45.6", with "–" for a leg that has no reading. */
function fmtPair(a, b, digits) {
    const one = (v) => (Number.isFinite(v) ? v.toFixed(digits) : '–');
    return Number.isFinite(b) ? one(a) + ' / ' + one(b) : one(a);
}

function fmtOne(v, digits) {
    return Number.isFinite(v) ? v.toFixed(digits) : '–';
}

/** #rrggbb + alpha → rgba(). Only ever fed the lane constants above. */
function hexA(hex, a) {
    const n = parseInt(hex.slice(1), 16);
    return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`;
}
