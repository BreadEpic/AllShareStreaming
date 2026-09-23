"""One cadence pass: does the stream, at the client's own refresh rate, repeat or
skip pictures because the host's clock and the client's drift apart?

    python cadence.py --fps 120 --vsync off --secs 150 --tag vdd-120-off
    python cadence.py --fps 60 --vsync on --target display --display-index 0 \
        --content-rect 0,0,2560,1440 --tag d1-60-on
    python analyze.py vdd-120-off [-v]

A self-stream of this machine, driven like the acceptance run (../acceptance):
the bench Chrome client, a scrolling page on the captured screen, then hook.js
in the client page, which stamps every refresh, every frame handed to the
decoder and every frame drawn. Raw stamps go to bench-out/cadence/raw-<tag>.jsonl.

The environment is the acceptance run's: MW_BENCH_LOCAL_PORTS for a --dev
instance, MW_BENCH_CLIENT_POS / MW_BENCH_CLIENT_LUID to put the client on a
screen that really runs at the rate under test, driven by another GPU than the
encoder's (docs/bench-campaign.md §4). See README.md for what a pass can and
cannot tell.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(BENCH))
OUT = os.path.join(REPO, "bench-out", "cadence")
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
import run, drive, fleet  # noqa: E402


def monitors():
    txt = subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(BENCH, "monitors.ps1")],
                         capture_output=True, text=True).stdout
    return [line.split() for line in txt.splitlines() if line.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fps", type=int, default=120)
    ap.add_argument("--vsync", choices=["on", "off"], default="off",
                    help="on = tearing off: the client paints on its refresh")
    ap.add_argument("--secs", type=int, default=150, help="measured time; cover two beats")
    ap.add_argument("--settle", type=int, default=8)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--target", default="vdisplay", help="vdisplay | display")
    ap.add_argument("--display-index", default="0", help="with --target display: app-id order")
    ap.add_argument("--content-rect", default="",
                    help="x,y,w,h of the captured screen; default: the Virtual Display")
    a = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, "raw-%s.jsonl" % a.tag)
    if os.path.exists(out):
        os.remove(out)

    access = dict(run.access_map().get("local") or {})
    probe = fleet.probe("local")
    pin = (probe.get("pin") or {}).get("pin")
    if pin:
        access["pin"] = pin
    access["lan"] = fleet.lan_url("local", probe) or access.get("lan")
    run.kiosk_start(access["lan"])
    d = drive.Driver(run.DEBUG_PORT)
    shown = False
    try:
        d.navigate(access["lan"])
        d.wait_library(access.get("name", "bench"), access.get("pin", ""), tries=25)
        settings = dict(run.load_matrix()["base"])
        settings.update({"stream_resolution": "fixed", "stream_height": 1080, "stream_fps": a.fps,
                         "tearing_default_v2": True, "tearing_enabled": a.vsync == "off"})
        d.apply_settings(settings)
        d.wait_library(access.get("name", "bench"), access.get("pin", ""), tries=25)
        os.environ["MW_BENCH_DISPLAY"] = a.display_index
        card, app = d.pick_tile(a.target)
        print("tile", card.get("name"), "/", app.get("name"), flush=True)
        before = {m[0] for m in monitors()}
        d.launch(card, app)
        d.wait_picture(timeout=60)
        time.sleep(2)

        # The Virtual Display only exists once the stream is up: it is the
        # screen that was not there before the launch.
        rect = a.content_rect
        if not rect:
            mons = monitors()
            vdd = [m for m in mons if m[0] not in before]
            if not vdd:
                raise SystemExit("no virtual display among %s — give --content-rect" % mons)
            x, y = vdd[0][1].split(",")
            w, h = vdd[0][2].split("x")
            rect = "%s,%s,%s,%s" % (x, y, w, h)
        os.environ["MW_BENCH_CONTENT_RECT"] = rect
        run.content_start("scroll.html", probe=False)
        shown = True
        time.sleep(4)
        print("hook", d.eval(open(os.path.join(HERE, "hook.js"), encoding="utf-8").read()),
              flush=True)
        time.sleep(a.settle)

        # A content page that does not present at the stream's rate measures the
        # page, not the clocks: check it, and put it up again if it lags.
        for attempt in range(3):
            d.eval("window.__mwc.dec.length = 0; window.__mwc.on = true")
            time.sleep(3)
            n = int(d.eval("String(window.__mwc.dec.length / 2)") or 0)
            d.eval("window.__mwc.on = false; for (const k of ['raf', 'dec', 'draw'])"
                   " window.__mwc[k].length = 0")
            print("check %d: %.1f fps received" % (attempt, n / 3), flush=True)
            if n / 3 >= 0.95 * a.fps:
                break
            run.content_start("scroll.html", probe=False)
            time.sleep(a.settle)

        d.eval("window.__mwc.on = true")
        t0 = time.time()
        chunk = 0
        while time.time() - t0 < a.secs:
            time.sleep(max(0, min(20, a.secs - (time.time() - t0))))
            data = d.json_eval("(() => { const C = window.__mwc; return JSON.stringify({"
                               "raf: C.raf.splice(0), dec: C.dec.splice(0), draw: C.draw.splice(0),"
                               "other: C.other, vis: document.visibilityState}); })()")
            data["chunk"] = chunk
            chunk += 1
            with open(out, "a", encoding="utf-8") as f:
                f.write(json.dumps(data) + "\n")
            print("chunk %d: %d refreshes, %d frames, %d drawn (%s)" % (
                chunk, len(data["raf"]), len(data["dec"]) // 2, len(data["draw"]) // 2,
                data["vis"]), flush=True)
        d.expand_latency_detail()
        with open(out, "a", encoding="utf-8") as f:
            f.write(json.dumps({"stats": d.stats(), "args": vars(a)}) + "\n")
    finally:
        try:
            d.stop()
        except Exception:
            pass
        if shown:
            run.content_stop()
        run.kiosk_stop()
    print("raw stamps in", out, flush=True)


if __name__ == "__main__":
    main()
