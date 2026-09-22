# mw-gpu-load — a game that takes the whole GPU, on demand

A neon torus knot of hexagonal panels, wrapped in shells and a volumetric glow,
spinning above a synthwave grid. Its cost is tuned until it renders at about
**45 fps** on **one chosen GPU**, then frozen. That fills the GPU queue the way a
AAA game does, so a stream encoded on the same GPU shows how it copes.

It is a lab instrument: built on demand, never installed, never shipped.

## Safeguards (not options)

- **60 s at most.** `--duration` accepts a shorter run and nothing longer.
- **Heat.** The GPU temperature is read twice a second, and the run stops *at
  once* when it reaches the limit: nothing more is submitted, and the device is
  released. The default limit is 85 °C, `--max-temp` accepts at most 90, and a
  driver that declares its own maximum lowers the limit to 3 °C under it (the
  RTX 5060 Ti says 87, so its limit is 84). A sensor that stops answering
  mid-run also stops the run.
  - **Windows:** D3DKMT adapter perf data, the counter Task Manager reads, for
    any vendor. On DualRTX, the Arc A380, the RTX 5060 Ti and the AMD iGPU all
    answer.
  - **Linux:** the GPU's hwmon (amdgpu, i915, xe), or NVML for the NVIDIA
    driver.
  - **macOS:** there is no public GPU sensor, so the tool uses the system
    thermal state and stops at `serious`.
- **No sensor, no run**, unless "No sensor: allow 60 s" is ticked
  (`--allow-no-sensor`). A red banner then says the run has no heat check.

## Build

The tool needs Qt 6.6 or later with the **qtshadertools** module
(`aqt install-qt <os> desktop <ver> <arch> -m qtshadertools`). It builds on its
own, without the backend's dependencies:

```
cmake -S backend/native-host/tools/gpu-load -B build-gpuload -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt>/<arch>
cmake --build build-gpuload
windeployqt --release build-gpuload/mw-gpu-load.exe     # Windows only
```

In the main build it comes with `-DMW_BUILD_TOOLS=ON`.

| OS | API | How the GPU is chosen |
|---|---|---|
| Windows | D3D12 (`MW_GPU_LOAD_API=d3d11` to compare) | adapter LUID |
| Linux | Vulkan | physical device |
| macOS | Metal | system default device |

## Use

```
mw-gpu-load                                    # window: GPU list, Start/Stop, Auto 45 fps
mw-gpu-load --list                             # index, name, VRAM, LUID, display
mw-gpu-load --gpu "RTX" --autostart --json run.jsonl
mw-gpu-load --gpu 0:73665 --autostart --level 93
```

- `--gpu` accepts an index, a LUID (`high:low`, or the low part in decimal), or
  part of the name. The name is the one MoonlightWeb reports for a display's
  encoder in `/api/native/status`.
- On Windows the window moves to the display that GPU drives, which is the
  display MoonlightWeb captures when it encodes there.
- `--json` writes one line per second (`fps`, `gpuMs`, `level`, `tempC`), then a
  `calibrated` line once the level is frozen, and an `end` line whose `reason`
  is `timeout`, `thermal`, `sensor-lost`, `user`, `device-lost` or
  `device-error`.
- Exit codes: 0 means ended normally, 2 thermal, 3 device, 4 refused (no GPU,
  no sensor, already hot).
- Calibration lasts 8 s. The level is then **frozen**, because a controller still
  running during a stream would hand back to the encoder exactly the GPU time
  the test means to take. To compare "load alone" with "load plus stream" at
  identical cost, replay the calibrated level with `--level`.

## Calibrated levels (22/09/2026)

| GPU | Level at ~45 fps | GPU time |
|---|---|---|
| AMD Radeon iGPU (DualRTX) | 4 (the floor) | 22 ms |
| Intel Arc A380 | 83–93 | 15–17 ms |
| NVIDIA RTX 5060 Ti | ~500 | 22 ms |
| Radeon 780M (UM790Pro, Ubuntu, Vulkan) | 248 | 21 ms |
| Apple M1 Pro (Metal) | 24 | — |

A level only compares on the same machine and window size: the glow costs per
pixel.

## Pitfalls met

- **Explicit scissor.** Without a scissor rectangle on the pipelines, Qt's D3D12
  backend drew nothing on the Arc and the RTX: every pixel was clipped away,
  while the clear still happened. On AMD the scene appeared. The failure looked
  like a rendered black window, and it cost the GPU almost nothing (0.7 ms). Run
  with `MW_GPU_LOAD_DEBUG=1` to turn on the debug layer and clear the window to
  magenta: that tells "nothing drawn" from "drawn black".
- **Screen captures by GDI** (`CopyFromScreen`) can miss what a flip-model swap
  chain shows. Desktop Duplication (`ffmpeg -f lavfi -i ddagrab`) sees what the
  encoder sees.
