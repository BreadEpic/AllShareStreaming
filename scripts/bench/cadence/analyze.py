"""Read a cadence pass: the two clocks, and every repeated or skipped picture
with the likeliest cause.

    python analyze.py <tag> [<tag> ...] [-v]

One JSON line per pass; -v adds one line per second (arrival phase, its spread,
repeats, skips, host gaps, missed refreshes).

What is counted, refresh by refresh of the client's screen: the frames drawn
between two refreshes. None is a REPEAT (the previous picture stays up), more
than one is a SKIP (all but the last are never seen). With tearing on, "between
two refreshes" uses the rAF stamps as the boundary; the compositor's real latch
sits a fixed offset away, so the counts stand but the phase they happen at is
shifted.

Each second's events are put down to one cause, first match wins:
  host    the host sent fewer frames than its clock says (a gap in backendTs)
  client  Chrome itself skipped a refresh (a rAF interval of two periods)
  phase   neither: a frame landed on the other side of a refresh boundary — the
          clocks drifting across it, or the decode time jittering around it.
The arrival phase is where, in the client's refresh period, frames reach the
decoder; its slope is the drift the two rates predict, in cycles per second.
"""
import bisect
import json
import math
import os
import statistics
import sys

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))), "bench-out", "cadence")
# A frame drawn in the next refresh's own callback is stamped at that refresh's
# time to the browser's 0.1 ms: boundaries 1 ms early keep it in its refresh.
EDGE_MS = 1.0


def load(tag):
    raf, dec, draw, tail = [], [], [], {}
    with open(os.path.join(OUT, "raw-%s.jsonl" % tag), encoding="utf-8") as f:
        for line in f:
            o = json.loads(line)
            if "stats" in o:
                tail = o
                continue
            raf += o["raf"]
            dec += [(o["dec"][i], o["dec"][i + 1]) for i in range(0, len(o["dec"]), 2)]
            draw += [(o["draw"][i], o["draw"][i + 1]) for i in range(0, len(o["draw"]), 2)]
    return sorted(raf), sorted(dec), sorted(draw), tail


def fit_period(times):
    """Period of a tick train with some ticks missing: each tick is numbered by
    the rounded gap to the previous one, then a least-squares line."""
    d = [b - a for a, b in zip(times, times[1:])]
    m = statistics.median(d)
    idx = [0]
    for x in d:
        idx.append(idx[-1] + max(1, round(x / m)))
    n = len(times)
    mx, my = sum(idx) / n, sum(times) / n
    slope = (sum((i - mx) * (t - my) for i, t in zip(idx, times))
             / sum((i - mx) ** 2 for i in idx))
    resid = [t - (my + slope * (i - mx)) for i, t in zip(idx, times)]
    return slope, idx[-1] + 1 - n, statistics.pstdev(resid)


def circular(xs):
    c = sum(math.cos(2 * math.pi * p) for p in xs) / len(xs)
    s = sum(math.sin(2 * math.pi * p) for p in xs) / len(xs)
    return (math.atan2(s, c) / (2 * math.pi)) % 1.0, math.hypot(c, s)


def analyze(tag, verbose=False):
    raf, dec, draw, tail = load(tag)
    args = tail.get("args", {})
    Pc = fit_period(raf)[0]
    fc = 1000 / Pc
    # The host's rate from its regular stretches only: a burst of missing
    # frames bends a whole-pass fit.
    ts = [t / 1000.0 for _, t in dec]
    win = int(10 * fc)
    rates = []
    for i in range(0, max(0, len(ts) - win), win):
        P, missing, res = fit_period(ts[i:i + win])
        if res < 1.0 and missing <= 2:
            rates.append(1000 / P)
    # None clean (a short pass with a gap in every window): no host rate, and
    # gaps are judged against the client's period, which is within a hair of it.
    fh = statistics.median(rates) if rates else float("nan")
    Ph = 1000 / fh if rates else 1000 / fc

    t0 = raf[0]
    secs = {}

    def sec(t):
        return secs.setdefault(int((t - t0) // 1000),
                               {"R": 0, "S": 0, "gaps": 0, "missed": 0, "ph": []})

    for i in range(len(ts) - 1):
        if ts[i + 1] - ts[i] > 1.5 * Ph:
            sec(dec[i + 1][0])["gaps"] += round((ts[i + 1] - ts[i]) / Ph) - 1
    drawn = [t for t, _ in draw]
    for k in range(len(raf) - 1):
        span = max(1, round((raf[k + 1] - raf[k]) / Pc))
        n = (bisect.bisect_left(drawn, raf[k + 1] - EDGE_MS)
             - bisect.bisect_left(drawn, raf[k] - EDGE_MS))
        b = sec(raf[k])
        b["missed"] += span - 1
        if n < span:
            b["R"] += span - n
        elif n > span:
            b["S"] += n - span
    for t, _ in dec:
        k = bisect.bisect_right(raf, t) - 1
        if 0 <= k < len(raf) - 1:
            sec(t)["ph"].append(((t - raf[k]) / Pc) % 1.0)

    rows, tot = [], {"host": [0, 0], "client": [0, 0], "phase": [0, 0]}
    for i in sorted(secs):
        b = secs[i]
        if not b["ph"]:
            continue
        mean, spread = circular(b["ph"])
        cause = "host" if b["gaps"] else "client" if b["missed"] else "phase"
        tot[cause][0] += b["R"]
        tot[cause][1] += b["S"]
        rows.append((i, mean, spread, b))
    minutes = len(rows) / 60

    # Drift of the arrival phase over the seconds where it is well defined.
    unw = []
    for i, mean, spread, b in rows:
        if spread < 0.7 or b["gaps"]:
            continue
        if unw:
            step = mean - (unw[-1][1] % 1.0)
            unw.append((i, unw[-1][1] + step - round(step)))
        else:
            unw.append((i, mean))
    slope = 0.0
    if len(unw) > 2:
        mx = sum(i for i, _ in unw) / len(unw)
        my = sum(p for _, p in unw) / len(unw)
        slope = (sum((i - mx) * (p - my) for i, p in unw)
                 / sum((i - mx) ** 2 for i, _ in unw))

    rows_out = tail.get("stats", {}).get("rows", {})
    out = {
        "tag": tag, "fps": args.get("fps"), "vsync": args.get("vsync"),
        "target": args.get("target", "vdisplay"), "seconds": len(rows),
        "client_hz": round(fc, 4), "host_hz": round(fh, 4), "diff_hz": round(fc - fh, 4),
        "beat_s": round(1 / abs(fc - fh), 1) if rates and fc != fh else None,
        "arrival_phase_slope": round(slope, 4),
        "per_min": {k: {"repeats": round(v[0] / minutes, 1), "skips": round(v[1] / minutes, 1)}
                    for k, v in tot.items()},
        "seconds_with_phase_events": sum(1 for _, _, _, b in rows
                                         if not b["gaps"] and not b["missed"] and (b["R"] or b["S"])),
        "overlay": {"fps": rows_out.get("Framerate:"), "latency": rows_out.get("Latency:")},
    }
    print(json.dumps(out))
    if verbose:
        for i, mean, spread, b in rows:
            print("%4d  phase %.3f  spread %.2f  R %-3d S %-3d gaps %-3d missed %-3d" % (
                i, mean, spread, b["R"], b["S"], b["gaps"], b["missed"]))
    return out


if __name__ == "__main__":
    for tag in [a for a in sys.argv[1:] if a != "-v"]:
        analyze(tag, "-v" in sys.argv)
