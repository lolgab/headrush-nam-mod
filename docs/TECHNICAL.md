# Technical design: the Anxiety OD hijack

Takes a stock HeadRush Pedalboard, MX5, or Gigboard firmware update file (`Update.img`)
and produces a modified one that hijacks the **Anxiety OD (v1)** pedal's
`process()` function, replacing its overdrive DSP with NAM inference. The
model is auto-detected from the `Update.img`'s `compatible` string (see
`core/model_targets.c` / `patch/model_targets.py`).

The GUI/CLI also offer an opt-in **"up to 4 instances"** mode that additionally
hijacks the separate **Anxiety OD V2** pedal (internal class name
`AnxietyV2` -- a distinct, separately-instantiable pedal type from v1, each
addable up to twice on a board) the same way. Both hijacks, when enabled,
patch the same stock `Evil` binary independently -- see "Up to 4 instances"
below.

The build refuses to proceed if the selected model's addresses don't match
the `Evil` binary in the `Update.img` (the Anxiety OD vtable slot must
already hold the expected `process()` address), so a wrong model / failed
auto-detect fails loudly rather than producing a bad image.

The MX5 and Gigboard ports reuse everything structural (same RK3288 armv7
hard-float non-PIE `Evil`, same FIT/ext2/launcher layout, same Anxiety OD v1
sacrificial pedal); only the absolute addresses differ, re-derived from each
`Evil` the way `patch/patch_gonkulator.py`'s docstring documents. On-screen
knob labels stay stock **Drive / Tone / Level** on all three devices.

## Knob implementation

Anxiety OD's on-screen labels stay stock (**Drive / Tone / Level**) on every
device, but do this instead:

| Original | Function |
|---|---|
| Drive | selects/scans `.nam` model files |
| Tone | input trim |
| Level | output trim |
| Hi-Lo | doesn't do anything |

The Model knob has a fixed 101 positions (0%-100%, one per whole percent).
Each position selects one `.nam` file, in alphabetical order, starting at 0%:
with N files found, positions `0..N-1` (percent) each play a file and
positions `N..100` are unmapped and render silence — not the previous/nearest
file, and not dry passthrough. This keeps a given file's knob position stable
across boots regardless of how many other files are present, instead of the
old scheme of dividing the sweep into N equal zones (which shifted every
file's position whenever one was added or removed). The `/NAM` folder is
scanned once, the first time Anxiety OD's `process()` runs — but that now
happens regardless of bypass state, not gated behind the pedal's first
non-bypass engage. After that, switching models is instant — it re-parses
already-cached JSON, no disk I/O.

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
Every thread that runs NAM inference (the real-time audio thread and this
library's own background load/prewarm thread) also sets the ARM VFP/NEON
flush-to-zero bit on entry -- `-ffast-math`'s own runtime support only sets
it on whichever thread happens to run this `.so`'s static initializers, not
on Evil's pre-existing audio thread or on threads we spawn ourselves (this
bit is per-thread, never inherited). Left unset, a WaveNet's hidden state
decaying toward silence during the fade/prewarm hits denormals, which take
a slow microcoded path on ARM and can stall a real-time callback past its
deadline -- same mechanism as
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

## Up to 4 instances (optional Anxiety OD V2 hijack)

By default the build only hijacks Anxiety OD (v1), which the stock firmware's
block-add menu allows up to twice per board -- 2 NAM instances max. The GUI's
"Also use Anxiety OD V2 for NAM (up to 4 instances)" mod (the CLI's
`--mods nam,nam-4x`, or `--instances 4`) also
hijacks **Anxiety OD V2** (internal class `AnxietyV2`), a separate,
sibling pedal type also addable up to twice, for up to 4 total.

Both hijacks reuse the exact same mechanism (`core/elf_patch.c`'s
`nam_elf_patch_gonkulator`, called twice against the same in-memory `Evil`
buffer -- once per class's own engine vtable/`process()` address, see
`core/model_targets.h`'s `v2_engine_vtable_vaddr`/`v2_orig_process_fn`) and
the exact same compiled hook function, `nam_process_gonk` -- it already
dispatches per-instance by engine-object pointer (`state_for()` in
`patch/nam_hook.cpp`) regardless of which pedal class called in, so no
V2-specific hook code exists. On every device checked so far, Anxiety OD
V2's `process()` turned out to be the literal same compiled function as
v1's -- confirmed by disassembly, not assumed.

**Quality auto-recalibrates board-wide as instances are added.** All
hijacked instances (v1 and V2 together) share one real-time per-block CPU
budget. Adding a 3rd or 4th instance shrinks everyone's fair share, so every
already-playing instance is forced through the same silent duck-out/
reload/duck-in cycle a knob-driven model switch uses, and picks a new
quality tier sized to the smaller shared budget -- not just the newly-added
instance. This is a lazy, self-correcting design (`g_topology_epoch` in
`nam_hook.cpp`, bumped whenever a new instance is first seen; each
instance's cached quality tier is tagged with the epoch it was computed
under, so anything stale gets recalculated the next time it's touched)
rather than an eager board-wide sweep, to avoid taking any lock from the
real-time audio thread.

## Safety details baked into the hook

- **Per-instance state**: if you add Anxiety OD more than once on a board,
  each instance gets its own independent model/knob state — an earlier
  single-shared-state design corrupted audio ("crazy noise" on real
  hardware) when two instances fed the same WaveNet history buffers.
- **Adaptive quality tiers**: NAM "A2" models expose quality tiers trading
  CPU for fidelity. This benchmarks each model's tiers in the background
  (never the audio thread) and picks the highest one that fits this
  device's real-time budget — accounting for how many hijacked instances
  (Anxiety OD v1 and, if enabled, V2) are active at once, since they share
  one audio-callback deadline. Already-playing instances are retroactively
  recalibrated when a new instance joins and shrinks everyone's share —
  see "Up to 4 instances" above.
- **Corrupt-file tolerant**: a `.nam` file with bad JSON or an unsupported
  architecture is silently skipped, not fatal to the others; a file that
  passes JSON parsing but fails to build a real DSP graph is caught too
  (confirmed on real hardware this otherwise caused full-device reboots).
- **Dry passthrough**: audio passes through unaffected until the first
  model finishes loading, and bypass takes precedence over everything.
- Filters out `._*.nam` AppleDouble sidecar files Finder creates when
  copying to FAT/exFAT USB drives.

## What didn't make it in

- The knob-label QML patch has shipped twice (see `git log`) and **neither
  version takes effect on real hardware**. The first patched the wrong QML
  unit by proximity-guessing. The current `patch/patch_qml_labels.py` /
  `core/qml_patch.c` target the exact, uniquely-identified source blob and
  are byte-correct by offset/length checks, but real-hardware testing
  (2026-08-13) confirmed on-screen labels still stay stock **Drive / Tone /
  Level** on Pedalboard. Root cause not yet identified; not shipped or
  documented as a working feature.
- A pedal *title* rename (`patch/patch_pedal_title.py`) is written but
  **disabled** — real-hardware testing showed renaming that string breaks
  the pedal entirely (it's very likely used as an internal type-name lookup
  key, not just a label).

See [`ADDITIVE_PEDAL.md`](ADDITIVE_PEDAL.md) for a second, non-hijacking
pedal design that was researched but never shipped (unreachable from the
firmware's own UI).

## Repo layout

- `patch/nam_hook.cpp` — the actual NAM inference + knob-scan model
  selection + Input/Output trim + quality-tier calibration logic, compiled
  into `libnam_hook.so`.
- `patch/nam_preload.cpp` — tiny `LD_PRELOAD` shim (`libnam_preload.so`)
  that wires the compiled hook functions into the patched `Evil` binary's
  trampolines at process startup.
- `patch/patch_gonkulator.py` — the original Python derivation of the
  Anxiety OD (v1) hijack; superseded for the shipped build by
  `core/elf_patch.c` (validated byte-identical), kept for its docstring's
  full derivation of every address used, including the two prior targets
  that were tried and abandoned.
- `patch/patch_qml_labels.py` — the original Python derivation of the
  on-screen knob relabel attempt; superseded for the shipped build by
  `core/qml_patch.c` (validated byte-identical). Supersedes
  `patch/patch_knob_labels.py`. Neither version's relabel takes effect on
  real hardware; see "What didn't make it in" above.
- `patch/patch_pedal_title.py` — **not invoked by the build** — renames the
  pedal's display title; disabled after real-hardware testing broke the
  pedal (see above).
- `patch/trampoline_gonk.S` — the ARM32 trampoline injected at Anxiety OD's
  `process()` vtable slot.
- `patch/model_targets.py` — per-model/firmware registry of the values that
  differ per `Evil` (engine vtable + `process()` address for v1 and,
  optionally, V2; QML label offsets); reimplemented in `core/model_targets.c`
  for the shipped build.
- `patch/test_nam_gonk_e2e.c` — standalone e2e test harness for the shipped
  hijack (run under QEMU armv7 user-mode emulation, e.g.
  `docker run --rm --platform linux/arm/v7 -v $PWD:/work -w /work
  debian:bookworm-slim ./test_nam_gonk_e2e`, after building it and
  `libnam_hook.so` with the same cross-compiler flags the build uses).
- `nam_core/` — [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore)
  (MIT license), fetched by `scripts/fetch_nam_core.sh` (not committed to
  this repo, not a git submodule).
- `patch/patch_namloader.py`, `patch/patch_modfac_spy.py`,
  `patch/trampoline_naml.S`, `patch/trampoline_trim.S`,
  `patch/case92_stub.S`, `patch/test_nam_naml_e2e.c` — the additive
  (non-hijacking) pedal design and its supporting dispatch-tracing tool.
  Not invoked by the build. See [`ADDITIVE_PEDAL.md`](ADDITIVE_PEDAL.md).

See [`BUILDING.md`](BUILDING.md) for the GUI/CLI build itself (`core/`,
`app/`, `tools/`, `scripts/`).
