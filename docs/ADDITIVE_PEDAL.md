# The additive NAM pedals (NAM / NAM TWO)

Two genuinely new pedals, **NAM** and **NAM TWO** (two registry pairs: the
firmware builds every pedal as "X" and "X 2", so there are four instances,
"NAM", "NAM 2", "NAM TWO", "NAM TWO 2"), added to the Pedalboard 2.7
firmware without sacrificing any stock pedal. They appear in the *Distortion*
list, behave like any other pedal (add, move, bypass, save a rig) and run
Neural Amp Modeler audio. This is what `gui-core-cli` and the installer do by
default on the Pedalboard; the Anxiety OD hijack is the opt-in alternative
(`--anxiety-hijack`, "Also make Anxiety OD run NAM"), and the two can be
installed together. Confirmed on real hardware as far as the "Status" table
says; see there.

The hijack design (Anxiety OD) is described in [TECHNICAL.md](TECHNICAL.md);
the display side of this pedal (dB faders, Drive readout) is in
[NAM_PEDAL_DISPLAY.md](NAM_PEDAL_DISPLAY.md).

Code: `core/spy_patch.c` / `.h` (all firmware patches), `tools/gui_core_cli.c`
(`--additive`), `core/patch_pipeline.c` (`additive_nam`),
`core/launcher_script.c`, `patch/engine_stub.S`, `patch/nam_preload.cpp`,
`patch/nam_hook.cpp`, `tools/modfac_spy_cli.c` (experiments).

## How the pedal is put together

Everything below is a binary patch of `/usr/Evil/Evil` plus two preloaded
libraries. Nothing is hijacked: the new pedals are added next to the stock
ones.

1. **Registry.** The firmware builds every pedal prototype once at boot from
   tables indexed by a pedal *id*. The patcher extends those tables with the
   new names (pedals come in pairs "X" and "X 2") and moves the end of the id
   range up wherever the firmware hard-codes it; every patched site is checked
   against its expected stock value before it is written (`core/spy_patch.c`).
2. **Picker.** The block-type popup lists pedals *by name*, from per-category
   name lists built at startup, not from the id tables. A hook appends the new
   names to the Distortion list, so "NAM" and "NAM TWO" appear at its end (each
   one's "2" twin is made by adding it again). A pair can be appended to the
   Reverb list instead, through the same hook
   ([REVERB_PEDAL.md](REVERB_PEDAL.md)).
3. **Second instance logic.** The mapping from picker entry to pedal id is
   adjusted so the new pairs get stock pair behavior. A single entry owning
   four ids was tried (v22) and abandoned: the picker row greys out after two
   uses and the logic that does it was never found. The shipped design is
   therefore two pairs, as in the v23-v28 builds ("NAM A" / "NAM B" then, "NAM"
   / "NAM TWO" now), which gives four instances per rig. A build with a single
   pair ("NAM" / "NAM 2") was tried and on hardware only the "NAM" entry showed
   up; the cause was not found, and the two-pair layout is the one confirmed
   to work.
4. **Property nodes.** A pedal needs nodes in the state tree
   (`/Engine/Patch/...`, `/Engine/MixerCtrl/<name>/<param>`). Stock code
   creates them in code, so ids with no code got a blank, transparent,
   uneditable pedal. Hooks register the new names with Glorious Drive's
   parameter list (Drive, Level, Tone, Gain, Hi-Cut, On, Colour) and add the
   new pedals to the list of pedal storages the firmware activates at boot;
   without that, saving a rig crashed.
5. **Audio.** The new pedal type gets a copy of Glorious Drive's engine whose
   `process()` is redirected to our code. `libnam_preload.so` (injected with
   `LD_PRELOAD`, only into `/usr/Evil/Evil`) loads `libnam_hook.so` and
   connects `nam_process_eng` to it, using the env var
   `NAM_HOOK_SLOT_ENG_ADDR`. The block size on hardware is 48 frames.
   Per-engine state is kept by engine pointer.
6. **Knobs.** The pedal maps Drive = model, Level = input volume, Tone =
   output volume (the editor lists Level before Tone, so the input comes
   first). The default 0.5 must land on a real model, so the model knob sweeps
   the files by index.
7. **Display.** Level / Tone read in dB and Drive shows the model number and
   file name, by rewriting translator descs at runtime from a watcher thread
   in the hook library: [NAM_PEDAL_DISPLAY.md](NAM_PEDAL_DISPLAY.md).

## Status

| Piece | State |
|---|---|
| Registry, picker entry, two instances per pedal, rig save, FX-Loop | confirmed on hardware (v21, v23) |
| two pairs (four instances), as the earlier NAM A / NAM B build | confirmed (v23) |
| single pair "NAM" / "NAM 2" (`ids = 2` engine class) | **failed**: only "NAM" showed up; not shipped |
| the shipped layout, two pairs "NAM" / "NAM TWO" (same code path as NAM A / NAM B) | built; **not yet flashed on hardware** |
| combined with the Anxiety OD hijack | built; **not yet flashed on hardware** |
| NAM audio, level / input trim | confirmed (v25, v26) |
| dB faders on Level / Tone | confirmed (v27/v28 and later hot-deployed libs) |
| Calibration on the Gain Low/High switch (High = on): input + output normalization | confirmed on hardware |
| Drive readout (number + file name) | hot-deployed only; consistency of the last build unconfirmed |
| Captions DRIVE / LEVEL / TONE | cannot be changed (see NAM_PEDAL_DISPLAY.md) |
| Pedal art | missing: `img/NAM.png` falls back to `img/Other.png` |
| Patch committed / in the GUI | not committed; CLI only (`--additive`) |

## Open items

* Drive readout can show the previous model's name until the value moves again;
  needs a UI repaint trigger or a decision to show the number only.
* The patch is not in the shipped GUI pipeline's defaults and not committed.
* Art and captions for the pedal; a generalization to N pairs (the registry
  constants derive from the pair count, but the node-registration hooks still
  assume the ids 167.. of the pairs they are given and seven parameters (Glorious
  Drive's), which is fine for up to `NAM_MAX_PAIRS`).
* Debug logging (`[nam_disp]`, `[nam_eng]`) should be trimmed before release.
