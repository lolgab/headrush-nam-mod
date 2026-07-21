# HeadRush Pedalboard 2.7 — Neural Amp Modeler (NAM) mod

Reverse-engineered firmware mod for the HeadRush Pedalboard, adding
[Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore)
(NAM, MIT-licensed) neural-network amp-model inference as a pedal.

**Status**: a real, working NAM pedal today, via a hijacked pedal slot. A
second, *additive* pedal design (own pedal type, nothing sacrificed) also
exists but is **not applied by the build** — it's unreachable from Evil's
own UI. See [`ADDITIVE_PEDAL.md`](ADDITIVE_PEDAL.md) for that research.

## What this does

Takes a stock HeadRush Pedalboard **2.7** firmware update file (`Update.img`)
and produces a modified one that hijacks the **Anxiety OD (v1)** pedal's
`process()` function, replacing its overdrive DSP with NAM inference.

This is a *hijack*: Anxiety OD loses its real overdrive function board-wide,
on every instance. It's the pedal type this mod settled on sacrificing —
earlier targets were tried and rejected: Gonkulator/"Ring Mod" turned out to
be dead code (not wired to any UI page, so the hook never fired), and Volume
worked but is needed for its real job (expression-pedal control).

### The three knobs

Anxiety OD's on-screen labels are patched to match what they now do:

| Original | Relabeled | Function |
|---|---|---|
| Drive | **Model** | selects/scans `.nam` model files |
| Tone | **Inp** | input trim |
| Level | **Outp** | output trim |
| Hi-Lo | *(unchanged)* | not wired to anything |

**Model select**: drop `.nam` files into their own **`/NAM`** folder on the
USB drive (sibling to `Impulse Responses`, `Blocks`, `Rigs`, etc. — create it
yourself via the File Manager/USB transfer view if it doesn't exist yet).
This has to be its own folder rather than reusing `Impulse Responses` —
Evil's own IR-folder sync purges anything it doesn't recognize as a real IR
(i.e. non-`.wav`) every time the USB transfer view reopens, confirmed on real
hardware. `/NAM` is untouched by that sync.

Files are sorted alphabetically; the Model knob's full sweep divides into N
equal zones, one per file found — turn it to switch models. The folder is
scanned once, lazily, the first time the pedal is actually used (not at
boot — an eager boot-time scan made USB-transfer hangs worse on real
hardware). After that, switching models is instant — it re-parses already
cached JSON, no disk I/O.

**Trim knobs**: both are `[0, 1]` with 0.5 = unity/0dB, 1.0 = +12dB boost
(override via `NAM_TRIM_MAX_DB`), and below 0.5 fades linearly to true
silence at 0 — matching how a physical volume knob feels near its minimum.

**Model switching** uses a ~100ms duck-and-switch crossfade (fade out old
model, fade in new one already warmed up) rather than a hard cut, to avoid
an audible click.

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

## Prerequisites

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

## Setup

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

## Usage

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

## Flashing (manual — this repo does not do this for you)

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
the modified data) — but this was never tested against a real device by
this project's authors beyond what's noted above. Proceed at your own risk.

## Repo layout

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
