# HeadRush Pedalboard 2.7 — Neural Amp Modeler (NAM) mod

Reverse-engineered firmware mod for the HeadRush Pedalboard, adding
[Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore)
(NAM, MIT-licensed) neural-network amp-model inference as a pedal.

**Status**: a real, working NAM pedal today, via a hijacked pedal slot,
confirmed on real hardware.

## For users

### Quick start (macOS)

Turns the stock HeadRush Pedalboard 2.7 firmware updater into a NAM-modded
one, in one command:

```sh
git clone --recurse-submodules <this-repo-url>
cd headrush-nam-mod
./scripts/quickstart_mac.sh
```

This checks you have the required build tools installed (prints the exact
`brew install` commands and stops if anything's missing — it never installs
anything itself), downloads the official HeadRush Pedalboard 2.7 Mac
updater, patches it with the NAM mod, and drops two apps in your current
directory:

```
HeadRush Pedalboard 2.7 Firmware Updater (NAM mod).app   <- run this to flash
HeadRush Pedalboard 2.7 Firmware Updater.app             <- unmodified, keep for recovery
```

**Flashing:**

1. Put your device in firmware-update mode. See
   [this video](https://www.youtube.com/watch?v=6H90kbOCJG8) for a
   walkthrough.
2. Run the updater

**Using it**:

Drop `.nam` files into their own **`/NAM`** folder on the USB drive (sibling
to `Impulse Responses`, `Blocks`, `Rigs`, etc — create it yourself via the
File Manager/USB transfer view if it doesn't exist yet). Files are sorted
alphabetically and divided evenly across the Model knob's sweep — name them
with an index prefix so you know the order:

- 01 - Your first model.nam
- 02 - Your second model.nam
- 03 - Your third model.nam

on the device, add the **Anxiety OD** pedal.

| Knob | Function |
|---|---|
| **DRIVE** | selects/scans `.nam` model files |
| **TONE** | input trim |
| **LEVEL** | output trim |

## For developers / maintainers

### What this does

Takes a stock HeadRush Pedalboard **2.7** firmware update file (`Update.img`)
and produces a modified one that hijacks the **Anxiety OD (v1)** pedal's
`process()` function, replacing its overdrive DSP with NAM inference.

This is a *hijack*: Anxiety OD loses its real overdrive function board-wide,
on every instance. It's fine since there is Anxiety OS V2 that is still available.

#### Knob implementation

Anxiety OD's on-screen labels are patched to match what they now do (see
"Using it" under For users above for what each knob does day to day):

| Original | Function |
|---|---|
| Drive | selects/scans `.nam` model files |
| Tone | input trim |
| Level | output trim |
| Hi-Lo | doesn't do to anything |

The Model knob's full sweep divides into N equal zones, one per `.nam` file
found (sorted alphabetically). The `/NAM` folder is scanned once, the first
time Anxiety OD's `process()` runs — but that now happens regardless of
bypass state, not gated behind the pedal's first non-bypass engage (see
below). After that, switching models is instant — it re-parses already-
cached JSON, no disk I/O.

Loading at Evil startup itself (via the `LD_PRELOAD` constructor, before
Evil's own `main()` runs) was tried twice and reverted both times on real
hardware: once as an unconditional filesystem scan (made USB-transfer hangs
worse), and once as just spawning the background thread that early (broke
boot entirely — clicking every ~500ms, stuck on the splash screen; spawning
a thread that early in process startup is evidently unsafe on this device).
Instead, the load/duck-and-switch/prewarm logic in `nam_process_gonk` now
runs unconditionally on every `process()` call, with the bypass check moved
to right before the only place it actually needs to change behavior (the
produced audio) instead of gating the load itself. Anxiety OD commonly sits
bypassed on a board from power-on until footswitched, so gating the load
behind non-bypass was effectively still a lazy-at-first-use load in
disguise — this way the model is already loaded and warmed up (or very
close to it) by the time the user actually engages it, without needing any
new thread-spawn timing.

Trim range (`[0, 1]`, 0.5 = unity/0dB, 1.0 = +12dB boost, fades to true
silence below 0.5) is overridable via `NAM_TRIM_MAX_DB`.

Model switching is implemented as a ~100ms duck-and-switch crossfade (fade
out old model, fade in new one already warmed up) rather than a hard cut.
Every thread that runs NAM inference (the real-time audio
thread and this library's own background load/prewarm thread) also sets the
ARM VFP/NEON flush-to-zero bit on entry -- `-ffast-math`'s own runtime
support only sets it on whichever thread happens to run this `.so`'s static
initializers, not on Evil's pre-existing audio thread or on threads we spawn
ourselves (this bit is per-thread, never inherited). Left unset, a WaveNet's
hidden state decaying toward silence during the fade/prewarm hits denormals,
which take a slow microcoded path on ARM and can stall a real-time callback
past its deadline -- same mechanism as
[mixxxdj/mixxx#16126](https://github.com/mixxxdj/mixxx/issues/16126).
- Every thread this library spawns itself (the preload scan and the
  per-switch construct/calibrate/prewarm) runs at the lowest niceness —
  this device has one ARM core, shared with Evil's own real-time audio
  thread, and none of this background work is latency-sensitive. Reported
  on real hardware as distinct glitches landing right at model-construction
  time, both on Anxiety OD's first-ever engage and on every later switch
  (construction/prewarm always re-runs for a newly-selected model, cache or
  not). Lowering niceness costs nothing and gives the kernel every reason
  to always favor the audio thread under contention.

### Safety details baked into the hook

- **Per-instance state**: if you add Anxiety OD more than once on a board,
  each instance gets its own independent model/knob state — an earlier
  single-shared-state design corrupted audio ("crazy noise" on real
  hardware) when two instances fed the same WaveNet history buffers.
- **Adaptive quality tiers**: NAM "A2" models expose quality tiers trading
  CPU for fidelity. This benchmarks each model's tiers in the background
  (never the audio thread) and picks the highest one that fits this
  device's real-time budget — accounting for how many Anxiety OD instances
  are active at once, since they share one audio-callback deadline.
- **Corrupt-file tolerant**: a `.nam` file with bad JSON or an unsupported
  architecture is silently skipped, not fatal to the others; a file that
  passes JSON parsing but fails to build a real DSP graph is caught too
  (confirmed on real hardware this otherwise caused full-device reboots).
- **Dry passthrough**: audio passes through unaffected until the first
  model finishes loading, and bypass takes precedence over everything.
- Filters out `._*.nam` AppleDouble sidecar files Finder creates when
  copying to FAT/exFAT USB drives.

### What didn't make it in

- The knob-label QML patch shipped once already (see `git log`) with a
  version that had **zero effect on real hardware** — it patched the wrong
  QML unit by proximity-guessing. The current `patch_qml_labels.py` targets
  the exact, uniquely-identified source blob instead and is confirmed
  correct by offset/length checks, but hasn't yet been re-confirmed on real
  hardware since the fix.
- A pedal *title* rename (`patch_pedal_title.py`) is written but **disabled**
  — real-hardware testing showed renaming that string breaks the pedal
  entirely (it's very likely used as an internal type-name lookup key, not
  just a label).

### Prerequisites (manual build)

`scripts/quickstart_mac.sh` (see Quick start above) checks these for you.
To run `build.sh` directly instead:

```sh
brew install e2fsprogs u-boot-tools
brew tap messense/macos-cross-toolchains
brew install armv7-unknown-linux-gnueabihf
```

(`e2fsprogs`/`armv7-unknown-linux-gnueabihf` are keg-only — the build script
finds them under `/opt/homebrew/opt/...` automatically, no need to modify
your `PATH`.)

Only tested on macOS (Apple Silicon). Should work on Linux with the
equivalent packages, but the toolchain lookup in
`scripts/build_update_img.py`'s `Toolchain` class currently only checks the
Homebrew keg paths and bare `PATH` — patch that class if your distro puts
things elsewhere.

### Setup

```sh
git clone --recurse-submodules <this-repo-url>
cd headrush-nam-mod
# if you cloned without --recurse-submodules:
git submodule update --init --recursive
```

You'll also need a stock **HeadRush Pedalboard 2.7** firmware updater — get
`Update.img` out of the official `.app`/installer's `Contents/Resources/`
(Mac) or equivalent (Windows). This repo does not include or distribute
HeadRush's firmware.

### Usage

```sh
./build.sh /path/to/stock/Update.img /path/to/output/Update_nam.img
```

This never modifies the input file. It extracts the rootfs, patches the
`Evil` binary and its launcher script, rebuilds the two shared libraries
that carry the actual NAM inference code, repacks everything, and verifies
the result round-trips byte-exact before writing it out. Takes a few
minutes (most of it is cross-compiling NAM's DSP core).

Pass `--keep-work-dir` to leave the intermediate build directory in place
(printed at the start of the run) for inspection/debugging.

### Flashing (manual — if you built `Update.img` directly instead of using `quickstart_mac.sh`)

1. **Back up the original, unmodified `Update.img`** somewhere safe. You
   will want it if anything goes wrong.
2. Put the modified `Update.img` wherever the official HeadRush Updater
   app expects to find it (varies by updater version — check its
   `Contents/Resources/` or equivalent).
3. Run the updater with the device in firmware-update mode.
4. **Recovery, if a flash goes wrong**: hold footswitches 1 and 8
   (leftmost, counting left to right) while powering on to force
   firmware-update/recovery mode, then reflash the original `Update.img`.

The FIT image format has no cryptographic signature check on the rootfs
itself (only SHA1 integrity hashes, which `mkimage` recomputes correctly for
the modified data). Tested on a real HeadRush Pedalboard 2.7 device: NAM
inference works, and the device can be safely recovered back to stock
firmware via the footswitch recovery mode (see step 4 above, and
[this video](https://www.youtube.com/watch?v=6H90kbOCJG8) for a walkthrough).
Proceed at your own risk.

### Repo layout

- `scripts/quickstart_mac.sh` — one-command macOS path: checks tools,
  downloads the stock Mac updater, runs the build, and patches a copy of
  the updater app in place. See Quick start above.
- `scripts/build_update_img.py` — the actual build pipeline (extraction,
  patching, cross-compilation, repacking, verification).
- `scripts/fit_image.py` — minimal FDT/FIT image reader used to pull the
  splash/recoverysplash/rootfs blobs and metadata out of the input
  `Update.img`, and to regenerate the `.its` source for `mkimage`.
- `patch/nam_hook.cpp` — the actual NAM inference + knob-scan model
  selection + Input/Output trim + quality-tier calibration logic, compiled
  into `libnam_hook.so`.
- `patch/nam_preload.cpp` — tiny `LD_PRELOAD` shim (`libnam_preload.so`)
  that wires the compiled hook functions into the patched `Evil` binary's
  trampolines at process startup.
- `patch/patch_gonkulator.py` — hijacks Anxiety OD (v1)'s real `process()`.
  Name kept from an earlier (rejected) Gonkulator target; its docstring has
  the full derivation of every address used, including the two prior
  targets that were tried and abandoned.
- `patch/patch_qml_labels.py` — relabels Anxiety OD's on-screen Drive/Tone/
  Level knobs. Supersedes `patch/patch_knob_labels.py` (kept for history —
  it shipped once with no real effect; see "What didn't make it in" above).
- `patch/patch_pedal_title.py` — **not invoked by the build** — renames the
  pedal's display title; disabled after real-hardware testing broke the
  pedal (see above).
- `patch/trampoline_gonk.S` — the ARM32 trampoline injected at Anxiety OD's
  `process()` vtable slot.
- `patch/test_nam_gonk_e2e.c` — standalone e2e test harness for the shipped
  hijack (run under QEMU armv7 user-mode emulation, e.g.
  `docker run --rm --platform linux/arm/v7 -v $PWD:/work -w /work
  debian:bookworm-slim ./test_nam_gonk_e2e`, after building it and
  `libnam_hook.so` with the same cross-compiler flags `build_update_img.py`
  uses).
- `nam_core/` — [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore)
  (MIT license), vendored as a git submodule.
- `patch/patch_namloader.py`, `patch/patch_modfac_spy.py`,
  `patch/trampoline_naml.S`, `patch/trampoline_trim.S`,
  `patch/case92_stub.S`, `patch/test_nam_naml_e2e.c` — the additive
  (non-hijacking) pedal design and its supporting dispatch-tracing tool.
  Not invoked by the build. See [`ADDITIVE_PEDAL.md`](ADDITIVE_PEDAL.md).

## License

The patch scripts and glue code in this repo are licensed under the
[GNU GPLv3](LICENSE). `nam_core/` is MIT-licensed by its own upstream
project (Steven Atkinson).

This project reverse-engineers and modifies HeadRush Pedalboard firmware,
which is not affiliated with or endorsed by inMusic/HeadRush. Use at your
own risk; modifying your device's firmware may void its warranty.
