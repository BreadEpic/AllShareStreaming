# The cadence bench: two clocks, one stream

When the stream runs at the client's own refresh rate, the host's clock and the
client's are still two clocks. Where a frame lands in the client's refresh
period slides slowly. Each time that point crosses a refresh boundary, a picture
is shown twice and the next one never. This bench measures how often that
happens, and why.

```bash
# --dev instance on 18080/18443, MW_VDD_GPU set to the encoder's GPU
set MW_BENCH_LOCAL_PORTS=18080,18443
set MW_BENCH_CLIENT_POS=200,100          # a screen really at the rate under test
set MW_BENCH_CLIENT_LUID=0,73665         # the GPU driving that screen, not the encoder's
python cadence.py --fps 120 --vsync off --secs 150 --tag vdd-120-off
python analyze.py vdd-120-off -v
```

`cadence.py` drives one self-stream the way `../acceptance` does:

1. the bench Chrome client starts the stream;
2. `scroll.html` goes on the captured screen, one new picture per refresh;
3. `hook.js` goes into the client page.

It checks that the stream really arrives at the rate asked for before measuring,
then saves raw stamps to `bench-out/cadence/`. Nothing in the app changes. The
hook patches `drawImage`, `VideoDecoder.decode` and runs its own rAF loop, only
in the page it is injected into.

`analyze.py` gives:

- both rates: the client's from rAF, the host's from `backendTs`;
- the beat they predict, and the measured slide of the arrival phase;
- every repeat and skip, put down to the host (frames it never sent), the client
  (refreshes Chrome skipped), or the phase (a frame on the wrong side of a
  boundary).

## What a pass cannot tell

- **Chrome's rAF follows the DWM clock, not its own screen.** A client window on
  the 60 Hz AMD screen ticked at 119.98 Hz, the primary's rate. On one Windows
  PC, host and client are therefore rarely two independent clocks. At 120 Hz the
  Virtual Display often locked to the client's clock outright. Only the Virtual
  Display at 60 Hz drifted every time: 59.988–59.991 Hz against 60.000. A real
  drift between two panels needs a second machine as the client.
- With tearing on, the boundary used is the rAF stamp; the compositor's latch is
  a fixed offset away. Counts hold, phases are shifted.
- Only the main-thread Canvas2D path is hooked (the default). The worker,
  WebGL/WebGPU and `<video>` paths are not counted.

Results and reading: `docs/bench-native-host.md` §8m.
