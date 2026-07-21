# HeadRush Pedalboard 2.7 — Neural Amp Modeler (NAM) mod

Reverse-engineered firmware mod for the HeadRush Pedalboard, adding
[Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore)
(NAM, MIT-licensed) neural-network amp-model inference as a pedal.

**Status**: a real, working NAM pedal today (via a hijacked pedal slot). A
second, *additive* pedal design also exists in `patch/patch_namloader.py`
but is **not applied by the build** (see "Known limitation" below) — it's
unreachable from Evil's own UI and it live-patches a hot dispatch path for
zero payoff today, so the risk isn't worth it until the reachability problem
is solved. See "What's implemented" below before you flash anything.

## What this does

Takes a stock HeadRush Pedalboard **2.7** firmware update file (`Update.img`)
and produces a modified one with:

1. **A reachable NAM pedal today**, by hijacking the "Ring Mod" pedal slot
   (internal codename `Gonkulator`). Its process() function is replaced with
   NAM inference. Its 3 knobs are *supposed* to be relabeled NAM / Inp / Outp
   (was Rate / Mix / Tone) via `patch/patch_knob_labels.py`, but confirmed on
   real hardware **this patch has no visible effect** — the on-screen labels
   still show the original names. The patched bytes are QML *source* text
   embedded as a Qt resource; Evil almost certainly runs Qt Quick Compiler
   (`qmlcachegen`) — evidenced by the `.qml_compile_hash`/`.qtmetadata` ELF
   sections — which precompiles QML to bytecode ahead of time, so the app
   likely never re-reads this source text at runtime. Cosmetic-only bug,
   not dangerous, just not yet fixed; finding/patching the actual compiled
   representation is unsolved.

   Model files: drop `.nam` files into their own **`/NAM`** folder on the
   USB drive (sibling to `Impulse Responses`, `Blocks`, `Rigs`, etc. — create
   it yourself via the File Manager/USB transfer view if it doesn't exist
   yet). Originally this piggybacked the existing `Impulse Responses` folder,
   but Evil's own IR-folder sync logic purges anything it doesn't recognize
   as a real IR (i.e. non-`.wav`) every time the USB transfer view reopens —
   confirmed on real hardware, `.nam` files placed there get silently
   deleted. `/NAM` is untouched by that sync. Files sorted alphabetically;
   the NAM knob's full sweep divides into N equal zones, one per file found —
   turn it to switch models. Folder re-scanned live every time the knob
   moves, so adding/removing files doesn't need a reboot.

   **Trade-off**: whichever board slot you put "Ring Mod" in loses its real
   ring-modulator function — this is a hijack, not an addition.

2. **A genuinely additive "Neural Amp Modeler" pedal type** (design only,
   **not applied by this build**) — its own case in the firmware's effect
   factory, its own engine object, no existing pedal sacrificed. Built and
   validated structurally + under QEMU emulation, but **not yet reachable**:
   nothing in Evil's own UI knows how to construct it from the pedal-add
   menu or a saved preset yet, and never tested on real hardware. It also
   patches the `ModFac_construct` dispatch instruction that runs on *every*
   pedal construction (including whatever preset loads at boot) — real
   surgery to a hot path, for a pedal nothing can select yet. Not worth that
   risk for zero working functionality, so `build_update_img.py` no longer
   invokes `patch_namloader.py`. Script kept for future manual use if the
   reachability problem gets solved via UART. See "Known limitation" below.

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
the modified data) — but this was never tested against a real device this
project. Proceed at your own risk.

## Known limitation: the additive pedal is not applied

The additive "Neural Amp Modeler" pedal type (see #2 above) needs Evil's
own code to translate a saved preset's `type` string (or a pedal-add menu
selection) into a call to its internal effect-factory function with the
right type index. That translation code was never found through static
analysis alone — it needs a real device with root/UART access and a
debugger (breakpoint at the factory function, load any preset, read the
backtrace) to trace.

Because it's unreachable, `build_update_img.py` **does not run**
`patch/patch_namloader.py` — that script also overwrites a live dispatch
instruction inside `ModFac_construct` (a function called on every pedal
construction, including whatever preset loads at boot), and that's too much
risk to a hot path for a pedal nothing can select. If you get UART access
and trace the menu/DB translation, `patch/patch_namloader.py`'s module
docstring has the full technical design; the pedal itself (engine, vtables,
DSP hook, trim knobs) is already built and just needs a menu entry — at
which point you'd re-wire its call back into `build_update_img.py` and
re-validate under QEMU before trying it on real hardware.

## Repo layout

- `scripts/build_update_img.py` — the actual build pipeline (extraction,
  patching, cross-compilation, repacking, verification).
- `scripts/fit_image.py` — minimal FDT/FIT image reader used to pull the
  splash/recoverysplash/rootfs blobs and metadata out of the input
  `Update.img`, and to regenerate the `.its` source for `mkimage`.
- `patch/nam_hook.cpp` — the actual NAM inference + knob-scan model
  selection + Input/Output trim logic, compiled into `libnam_hook.so`.
- `patch/nam_preload.cpp` — tiny `LD_PRELOAD` shim (`libnam_preload.so`)
  that wires the compiled hook functions into the patched `Evil` binary's
  trampolines at process startup.
- `patch/patch_gonkulator.py` — hijacks the real Gonkulator/"Ring Mod"
  pedal's `process()`.
- `patch/patch_namloader.py` — **not invoked by `build_update_img.py`** (see
  "Known limitation" above). Builds the additive, non-hijacking pedal type
  (own `ModFac_construct` case, own engine/vtables). Its module docstring is
  the full technical writeup of how HeadRush's effect-factory binary format
  works.
- `patch/patch_knob_labels.py` — relabels the Ring Mod editor screen's 3
  knobs.
- `patch/trampoline_*.S`, `patch/case92_stub.S` — hand-written ARM32
  assembly trampolines the patch scripts inject.
- `patch/test_nam_*_e2e.c` — standalone e2e test harnesses (run under QEMU
  armv7 user-mode emulation, e.g. `docker run --rm --platform linux/arm/v7
  -v $PWD:/work -w /work debian:bookworm-slim ./test_nam_naml_e2e`, after
  building it and `libnam_hook.so` with the same cross-compiler flags
  `build_update_img.py` uses).
- `nam_core/` — [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore)
  (MIT license), vendored as a git submodule.

## License

The patch scripts and glue code in this repo have no license file yet —
treat as all-rights-reserved by the author until one is added. `nam_core/`
is MIT-licensed by its own upstream project (Steven Atkinson).

This project reverse-engineers and modifies HeadRush Pedalboard firmware,
which is not affiliated with or endorsed by inMusic/HeadRush. Use at your
own risk; modifying your device's firmware may void its warranty.
