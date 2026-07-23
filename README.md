# HeadRush Pedalboard & MX5 (2.7) — Neural Amp Modeler (NAM) mod

Reverse-engineered firmware mod for the HeadRush Pedalboard and MX5, adding
[Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore)
(NAM, MIT-licensed) neural-network amp-model inference as a pedal.

**Status**: a real, working NAM pedal today, via a hijacked pedal slot,
confirmed on real hardware — see [Supported models](#supported-models).

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

> This one-command macOS path currently fetches the **Pedalboard** updater only.
> For the **MX5** on macOS, extract `Update.img` from the MX5 Mac updater
> yourself and run `./build.sh` (see [Usage](#usage)) — it auto-detects the
> model. (The Linux/Windows quickstarts below take `--model mx5-2.7`.)

### Quick start (Linux)

Same idea, using Docker for the ARM cross-toolchain/e2fsprogs/u-boot-tools
instead of distro packages (these tools' on-disk-format compatibility is
version-sensitive enough that pinning them in an image beats hoping your
distro's versions behave):

```sh
git clone --recurse-submodules <this-repo-url>
cd headrush-nam-mod
# get Update.img out of the official Linux/generic updater's payload yourself,
# then:
./scripts/build_docker.sh /path/to/stock/Update.img Update_nam.img
```

Requires Docker (`docker info` must work). See "Docker-based build" under
For developers below for what this does; flashing steps are the same as the
manual macOS flow further down.

### Quick start (Windows)

Windows itself can't run the Linux/Docker toolchain directly, but WSL2 can —
and Docker Desktop's WSL2 backend makes `docker` available inside your WSL
distro with no extra setup
([docs](https://docs.docker.com/desktop/wsl/)). From a WSL2 terminal (e.g.
Ubuntu):

```sh
sudo apt install p7zip-full unzip git curl   # if not already present
git clone --recurse-submodules <this-repo-url>
cd headrush-nam-mod
./scripts/quickstart_windows.sh                 # HeadRush Pedalboard 2.7 (default)
./scripts/quickstart_windows.sh --model mx5-2.7 # HeadRush MX5 2.7
```

This checks required tools, downloads the official HeadRush **Windows** updater
`.exe` for the selected `--model` (default `pedalboard-2.7`; also `mx5-2.7` —
see [Supported models](#supported-models)), builds the NAM-modded `Update.img`
via Docker, and repacks a new installer `.exe` (see "Patching the Windows
updater .exe" under For developers for how that repacking works). It leaves two
files in the current directory, e.g. for the default Pedalboard:

```
HeadRush Pedalboard 2.7 Firmware Updater - Win (NAM mod).exe   <- copy to Windows, run this to flash
HeadRush Pedalboard 2.7 Firmware Updater - Win.exe             <- unmodified, keep for recovery
```

If you built from WSL, copy the patched `.exe` to the Windows side (it's
already on your Windows filesystem if your WSL working directory is under
`/mnt/c/...`) and run it there like the official updater. It's unsigned
(see that section for why) — Windows SmartScreen may warn about an
unrecognized publisher, which is expected for any unofficial build.

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

On the **Pedalboard** the on-screen labels are relabeled to match (Model / Inp /
Outp); on the **MX5** they keep their original **Drive / Tone / Level** names but
do exactly the same thing (see [Supported models](#supported-models)).

## For developers / maintainers

### What this does

Takes a stock HeadRush Pedalboard or MX5 **2.7** firmware update file
(`Update.img`) and produces a modified one that hijacks the **Anxiety OD (v1)**
pedal's `process()` function, replacing its overdrive DSP with NAM inference.
The model is auto-detected (see [Supported models](#supported-models)).

This is a *hijack*: Anxiety OD loses its real overdrive function board-wide,
on every instance. It's fine since there is Anxiety OS V2 that is still available.

#### Knob implementation

Anxiety OD's on-screen labels are patched (on the **Pedalboard** only — the MX5
keeps its stock labels, see [Supported models](#supported-models)) to match what
they now do (see "Using it" under For users above for what each knob does day to
day):

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

On Debian/Ubuntu (including WSL2) **use the Docker build below**, not a
host-installed cross-toolchain. Debian bookworm's `crossbuild-essential-armhf`
(and Ubuntu's `arm-linux-gnueabihf-g++`) target **glibc 2.36/2.35**, which links
the mod's shared libs against `GLIBC_2.33`/`GLIBC_2.34` symbol versions the
device (**glibc 2.32**) can't resolve — the flashed device then **hangs on the
boot splash** (recoverable, but wasted). You need a cross-toolchain targeting
**glibc ≤ 2.32**; the macOS Homebrew one already does, and the Docker image
below fetches Bootlin's glibc-2.31 one. `scripts/build_update_img.py`'s
`Toolchain` class finds the Homebrew (`armv7-unknown-linux-gnueabihf-*`),
Bootlin/Docker (`arm-buildroot-linux-gnueabihf-*`), and Debian
(`arm-linux-gnueabihf-*`) namings; as a backstop it **refuses to package libs
that need newer glibc symbols than the target device provides** (read straight
out of the rootfs), so a too-new toolchain fails the build instead of bricking.

### Docker-based build (recommended for Linux and Windows/WSL)

`debugfs`/`mkimage`'s on-disk-format compatibility is version-sensitive
enough that pinning exact tool versions in an image beats hoping a given
host distro's packages behave. `docker/Dockerfile` builds a Debian image
with the exact e2fsprogs/u-boot-tools/dtc versions this repo is tested
against, plus a **Bootlin glibc-2.31 ARM cross-toolchain** (deliberately not
Debian's `crossbuild-essential-armhf`, whose glibc 2.36 would produce libs the
glibc-2.32 device can't load — see the Dockerfile comment).
`scripts/build_docker.sh` builds that image (once, cached after) and runs the
same `build_update_img.py` inside it:

```sh
./scripts/build_docker.sh /path/to/stock/Update.img /path/to/output/Update_nam.img
```

Pass `--rebuild` as the first argument to force rebuilding the image (e.g.
after pulling changes to `docker/Dockerfile`). This is exactly what
`scripts/quickstart_windows.sh` uses under the hood, and works identically
on macOS, Linux, and Windows/WSL2 (anywhere Docker Desktop or a Linux docker
engine is available) — same image, same tool versions, same output,
regardless of host OS.

### Supported models

Per-model addresses live in `scripts/model_targets.py`. The build auto-detects
the model from the `Update.img`'s `compatible` string, or you force it with
`--model`:

| `--model` | Device | Firmware | `compatible` | Status |
|---|---|---|---|---|
| `pedalboard-2.7` | HeadRush **Pedalboard** | 2.7 | *(default)* | confirmed on real hardware |
| `mx5-2.7` | HeadRush **MX5** | 2.7 | `inmusic,hg04` | confirmed on real hardware |

**Only flash the image built for _your exact model and firmware_** — flashing
another model's image will almost certainly brick the device. The build refuses
to proceed if the selected model's addresses don't match the `Evil` binary in
the `Update.img` (the Anxiety OD vtable slot must already hold the expected
`process()` address), so a wrong `--model`/auto-detect fails loudly rather than
producing a bad image.

The MX5 port reuses everything structural (same RK3288 armv7 hard-float non-PIE
`Evil`, same FIT/ext2/launcher layout, same Anxiety OD v1 sacrificial pedal);
only the absolute addresses differ, re-derived from the MX5 `Evil` the way
`patch/patch_gonkulator.py`'s docstring documents. One thing is not ported: the
on-screen knob relabel — the MX5's labels come from a shared string pool, not a
per-pedal QML blob, so on the MX5 the knobs keep their **Drive / Tone / Level**
names while doing model-select / input-trim / output-trim.

**MX5 recovery** differs from the Pedalboard's "footswitches 1 & 8": hold the
**first two footswitches** while powering on to force firmware-update mode, then
reflash the stock `Update.img`. Keep a stock copy before flashing.

### Setup

```sh
git clone --recurse-submodules <this-repo-url>
cd headrush-nam-mod
# if you cloned without --recurse-submodules:
git submodule update --init --recursive
```

You'll also need a stock **HeadRush** firmware updater for your model — get
`Update.img` out of the official `.app`/installer's `Contents/Resources/`
(Mac) or the Windows updater's payload (it's a 7-Zip self-extracting `.exe`).
This repo does not include or distribute HeadRush's firmware.

### Usage

```sh
./build.sh /path/to/stock/Update.img /path/to/output/Update_nam.img
# force a specific model instead of auto-detecting:
./build.sh /path/to/stock/Update.img /path/to/output/Update_nam.img --model mx5-2.7
```

The model is auto-detected from the `Update.img` (`--model` overrides — see
[Supported models](#supported-models)). This never modifies the input file. It
extracts the rootfs, patches the `Evil` binary (at the selected model's
addresses) and its launcher script, rebuilds the two shared libraries that carry
the actual NAM inference code, repacks everything, verifies the result
round-trips byte-exact, and checks the built libs' glibc symbol versions are
loadable on the target device before writing it out. Takes a few minutes (most
of it is cross-compiling NAM's DSP core).

Pass `--keep-work-dir` to leave the intermediate build directory in place
(printed at the start of the run) for inspection/debugging.

### Flashing (manual — if you built `Update.img` directly instead of using `quickstart_mac.sh`)

1. **Back up the original, unmodified `Update.img`** somewhere safe. You
   will want it if anything goes wrong.
2. Put the modified `Update.img` wherever the official HeadRush Updater
   app expects to find it (varies by updater version — check its
   `Contents/Resources/` or equivalent).
3. Run the updater with the device in firmware-update mode.
4. **Recovery, if a flash goes wrong**: force firmware-update/recovery mode by
   holding, while powering on, footswitches **1 and 8** on the Pedalboard
   (leftmost, counting left to right) or the **first two** footswitches on the
   MX5, then reflash the original `Update.img`.

The FIT image format has no cryptographic signature check on the rootfs
itself (only SHA1 integrity hashes, which `mkimage` recomputes correctly for
the modified data). Tested on real HeadRush Pedalboard 2.7 and MX5 2.7 devices:
NAM inference works, and the device can be safely recovered back to stock
firmware via the footswitch recovery mode (see step 4 above, and
[this video](https://www.youtube.com/watch?v=6H90kbOCJG8) for a Pedalboard
walkthrough). Proceed at your own risk.

### Patching the Windows updater .exe

The Windows updater isn't a `.app` bundle with `Update.img` sitting in a
resources folder — it's a plain **7-Zip self-extracting installer**: a PE
stub, a small text config block (standard 7-Zip SFX syntax:
`;!@Install@!UTF-8! ... RunProgram="FirmwareUpdater.exe" ... ;!@InstallEnd@!`),
then a normal 7z archive holding `Background.png`, `Config.json`,
`Update.img`, `FirmwareUpdater.exe`, and `libusb-1.0.dll` as flat files (no
subfolders). `Config.json` (device-detection UI strings) has no checksum of
`Update.img`, so `scripts/repack_windows_updater.py` just swaps that one
entry and rebuilds the archive:

```sh
python3 scripts/repack_windows_updater.py stock_updater.exe Update_nam.img output.exe
```

(`scripts/quickstart_windows.sh` runs this for you automatically after
building `Update_nam.img` via Docker.) One wrinkle: the stock `.exe` is
Authenticode-signed, with the signature appended as a certificate block
*after* the 7z archive, referenced by an absolute file offset baked into the
PE header's Security Directory entry. Since editing the archive changes the
file's total length, that stored offset would otherwise point into the
middle of the new archive instead of a valid certificate — and 7-Zip's own
PE parser refuses to recognize the whole file as an archive at all when it
can't read a valid certificate there (not just skip it). The script zeros
that directory entry, since there's no way to produce a signature that
verifies over modified bytes anyway; the repacked `.exe` is unsigned, same
as every other artifact this repo produces. Round-trip verified: `7z t`
integrity-checks the output, then re-extracts it and confirms
`Background.png`/`Config.json`/`FirmwareUpdater.exe`/`libusb-1.0.dll` are
byte-exact to stock and `Update.img` matches the patched input exactly.

### Repo layout

- `scripts/quickstart_mac.sh` — one-command macOS path: checks tools,
  downloads the stock Mac updater, runs the build, and patches a copy of
  the updater app in place. See Quick start above.
- `scripts/quickstart_windows.sh` — one-command Windows path (run from
  Linux or WSL2): checks tools, downloads the stock Windows updater `.exe`,
  builds via Docker, and repacks a patched `.exe`. See Quick start above.
- `docker/Dockerfile` — pinned Debian image with the ARM cross-toolchain,
  e2fsprogs, u-boot-tools, and device-tree-compiler this repo is tested
  against. See "Docker-based build" above.
- `scripts/build_docker.sh` — builds/caches the docker image and runs
  `build_update_img.py` inside it; used directly on Linux and by
  `quickstart_windows.sh`.
- `scripts/build_update_img.py` — the actual build pipeline (extraction,
  patching, cross-compilation, repacking, verification, glibc-compat check).
- `scripts/model_targets.py` — per-model/firmware registry of the values that
  differ per `Evil` (engine vtable + `process()` address, QML label offsets) that
  lets one pipeline target the Pedalboard 2.7 and MX5 2.7. Auto-detected by
  `compatible`.
- `scripts/repack_windows_updater.py` — swaps `Update.img` inside the
  Windows updater `.exe`'s embedded 7z archive for a patched one. See
  "Patching the Windows updater .exe" above.
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

This project reverse-engineers and modifies HeadRush Pedalboard and MX5
firmware, which is not affiliated with or endorsed by inMusic/HeadRush. Use at
your own risk; modifying your device's firmware may void its warranty.
