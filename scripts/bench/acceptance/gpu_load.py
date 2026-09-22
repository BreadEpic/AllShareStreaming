"""A game-like GPU load under a pass: mw-gpu-load on the encoder's own GPU.

A pass that carries `"load": "gpu"` starts the tool before the stream, waits
for it to calibrate (about 45 fps, then the level is frozen), streams as usual,
and records what the tool saw while the stream was measured. Compared with the
same pass without the load, the difference is what a game that takes the whole
GPU costs the stream (Resident Evil 9, 21/09).

The GPU is the one MoonlightWeb names for the target display in
/api/native/status: the same GPU as the encoder, by construction. The tool
itself refuses to run hotter than its limit and never runs longer than 60 s,
so the pass fits inside one run: ~9 s of calibration, then the stream.

Only the machine this runs on (DualRTX) for now. The tool is built with
`cmake -S backend/native-host/tools/gpu-load -B build-gpuload`; MW_GPU_LOAD
points at another binary.
"""
import json
import os
import subprocess
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DEFAULT_TOOL = os.path.join(REPO, "build-gpuload", "mw-gpu-load.exe")


class Unavailable(Exception):
    """The load cannot run for this pass; the reason says why."""


def _tool():
    tool = os.environ.get("MW_GPU_LOAD", DEFAULT_TOOL)
    if not os.path.exists(tool):
        raise Unavailable("mw-gpu-load is not built (%s)" % tool)
    return tool


def encoder_gpu(target, port=48080):
    """The GPU that encodes `target` on this machine, as MoonlightWeb names it."""
    if target != "display":
        # The virtual display exists only once a stream has turned it on, and
        # the load has to be running before that stream starts.
        raise Unavailable("the load needs a physical display target, not %r" % target)
    with urllib.request.urlopen("http://127.0.0.1:%d/api/native/status" % port, timeout=5) as r:
        status = json.load(r)
    displays = status.get("displays") or []
    if not displays or not displays[0].get("gpu"):
        raise Unavailable("the native host names no GPU for its first display")
    # pick_tile("display") streams the first physical display.
    return displays[0]["gpu"]


def read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return [json.loads(line) for line in f if line.strip()]
    except (OSError, ValueError):
        return []


class Load:
    def __init__(self, gpu, json_path, level=None):
        self.gpu = gpu
        self.path = json_path
        os.makedirs(os.path.dirname(json_path), exist_ok=True)
        argv = [_tool(), "--gpu", gpu, "--autostart", "--json", json_path]
        if level:
            argv += ["--level", str(level)]
        self.proc = subprocess.Popen(argv)
        self.started = time.time()

    def wait_calibrated(self, timeout=25):
        deadline = time.time() + timeout
        while time.time() < deadline:
            lines = read(self.path)
            for line in lines:
                if line.get("event") == "calibrated":
                    return line
                if line.get("event") in ("refused", "end"):
                    raise Unavailable("mw-gpu-load: %s %s" % (
                        line.get("reason", ""), line.get("detail", "")))
            if self.proc.poll() is not None:
                raise Unavailable("mw-gpu-load exited (%s) before calibrating" % self.proc.returncode)
            time.sleep(0.5)
        raise Unavailable("mw-gpu-load did not calibrate within %d s" % timeout)

    def snapshot(self, seconds):
        """What the tool saw over the last `seconds`: its frame rate, its GPU
        time, the heat, and whether it was still running at all."""
        lines = read(self.path)
        ticks = [l for l in lines if "fps" in l and l.get("phase") == "locked"]
        recent = ticks[-max(1, int(seconds)):]
        end = next((l for l in lines if l.get("event") == "end"), None)
        calibrated = next((l for l in lines if l.get("event") == "calibrated"), {})

        def mean(key):
            vals = [l[key] for l in recent if l.get(key) is not None]
            return round(sum(vals) / len(vals), 1) if vals else None

        return {
            "gpu": self.gpu,
            "level": round(calibrated.get("level", 0), 1),
            "fps": mean("fps"),
            "gpuMs": mean("gpuMs"),
            "tempC": max((l.get("tempC", -1) for l in recent), default=None),
            "running": end is None and self.proc.poll() is None,
            "end": end,
        }

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
