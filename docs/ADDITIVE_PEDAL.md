# The additive NAM pedals (NAM A / NAM B)

A genuinely new pair of pedals, **NAM A** and **NAM B** (two instances each,
four NAM instances in total), added to the Pedalboard 2.7 firmware without
sacrificing any stock pedal. They appear in the *Distortion* list, behave like
any other pedal (add, move, bypass, save a rig) and run Neural Amp Modeler
audio. Confirmed on real hardware; see "Status" for what is still open.

This replaces the earlier version of this document, which described a static
design (a new `ModFac_construct` case plus `patch_namloader.py`) that could
never be reached. That design is kept at the end as history. The shipped
hijack design (Anxiety OD) is still described in
[TECHNICAL.md](TECHNICAL.md); the display side of this pedal (dB faders, Drive
readout) is in [NAM_PEDAL_DISPLAY.md](NAM_PEDAL_DISPLAY.md).

Code: `core/spy_patch.c` / `.h` (all firmware patches), `tools/gui_core_cli.c`
(`--additive`), `core/patch_pipeline.c` (`additive_nam`),
`core/launcher_script.c`, `patch/engine_stub.S`, `patch/nam_preload.cpp`,
`patch/nam_hook.cpp`, `tools/modfac_spy_cli.c` (experiments).

## How the pedal is put together

Everything below is a binary patch of `/usr/Evil/Evil` (ARM32, non-PIE; file
offset = vaddr - 0x8000, RW segment offset = vaddr - 0x9000) plus two
preloaded libraries. Nothing is hijacked: the new pedals are added next to the
stock ones.

1. **Registry.** All pedal prototypes are built once at boot by the ModFac
   constructor (fcn `0x624ef0`) from parallel tables indexed by a pedal *id*
   (`0` = Empty Slot, `1..166` stock pedals, `167` = FX-Loop). The tables are
   relocated and extended with the new names; pedals come in pairs "X" and "X 2".
   Stock firmware hard-codes the id space in dozens of places (`cmp #0xa7`
   for FX-Loop, `0xa6` for the last normal id, loop bounds `0xa8`, ...). About
   73 compare/`mov` constants are bumped by the number of added ids
   (`k_id_sites` in `core/spy_patch.c`, each checked against its old word
   before writing). Tables extended: names (`0x1609f28`), short names
   (`0x185fcb8`), types, category table X (`0x1609c88`), P1-P4, T5, T7 (editor
   path table, `0x1862f30`). The new tables live in code-cave and RW-tail
   slack; the CLI grows the file by 4096 zero bytes for the RW part.
2. **Picker.** The block-type popup is not driven by the id tables. It lists
   pedals *by name* from literal lists built by a static initializer
   (`0x66288`): a `std::map<category, vector<string>>` (Distortion call site
   `0x665ac`, count `0x12`). A hook there rebuilds the array with the new
   names appended, so "NAM A" / "NAM B" appear at the end of Distortion. (The
   hook must initialize the destination string itself: `0x5b48e0` does not
   and crashes in libc otherwise.)
3. **Second instance logic.** Picker index `s` maps to ids `2s-1` / `2s` for
   `s <= 83`, with a presence check and a second-instance path; above that the
   tail does `id = s + 83` with no duplicate logic. The three constants in
   that function (`0x3cdd10`, `0x3cddd8`, `0x3cdddc`) are bumped so the new
   pairs get stock pair behavior. A single entry owning four ids was tried
   (v22) and abandoned: the picker row greys out after two uses and the logic
   that does it was never found. Two entries with two instances each (v23)
   work.
4. **Property nodes.** A pedal needs nodes in the state tree
   (`/Engine/Patch/...`, `/Engine/MixerCtrl/<name>/<param>`). Stock code
   creates them in code (`0x5f88a8` via `0x594ab4` / `0x5938bc`), so ids with no
   code got a blank, transparent, uneditable pedal. Three hooks (A/B/C, one
   616-byte blob, veneers in dead `ModFac_construct` case bodies) register the
   new names with Glorious Drive's descriptor table (`0x33ca244`, 7 params:
   Drive, Level, Tone, Gain, Hi-Cut, On, Colour). Hook C also runs at the entry
   of `FUN_00406014`, which activates a **hard-coded list of every pedal
   storage** through `5b3654`; the new storages must be added to that list or
   the node's value-source (child `+0x78`) stays unset and saving crashes.
5. **Audio.** The new type reuses a dead ModFac case (type 80). Its module
   vtable is a clone of Glorious Drive's; a 148-byte setup stub swaps the engine
   vtable for a clone whose `process()` slot (8) jumps through a trampoline to
   a slot in the RW tail (`0x33def90`). `libnam_preload.so` (injected with
   `LD_PRELOAD`, only into `/usr/Evil/Evil`) loads `libnam_hook.so` and fills
   the slot from the env var `NAM_HOOK_SLOT_ENG_ADDR` with `nam_process_eng`.
   The block size on hardware is 48 frames. Per-engine state is kept by
   engine pointer.
6. **Knobs.** The Glorious Drive engine floats are, by creation order, Level at
   `0x540`, Drive at `0x554`, Tone at `0x568`; the pedal maps
   Drive = model, Level = output volume, Tone = input volume. The default 0.5
   must land on a real model, so the model knob sweeps the files by index.
7. **Display.** Level / Tone read in dB and Drive shows the model number and
   file name, by rewriting translator descs at runtime from a watcher thread
   in the hook library: [NAM_PEDAL_DISPLAY.md](NAM_PEDAL_DISPLAY.md).

## Status

| Piece | State |
|---|---|
| Registry, picker entry, two instances per pedal, rig save, FX-Loop | confirmed on hardware (v21, v23) |
| NAM A + NAM B = four instances | confirmed (v23) |
| NAM audio, level / input trim | confirmed (v25, v26) |
| dB faders on Level / Tone | confirmed (v27/v28 and later hot-deployed libs) |
| Drive readout (number + file name) | hot-deployed only; consistency of the last build unconfirmed |
| Captions DRIVE / LEVEL / TONE | cannot be changed (see NAM_PEDAL_DISPLAY.md) |
| Pedal art | missing: `img/NAM A.png` falls back to `img/Other.png` |
| Patch committed / in the GUI | not committed; CLI only (`--additive`) |

## Findings and recipes

**Observation tools.** Root shell over USB CDC (`/dev/cu.usbmodem...0001`,
bare `\r\n` wakes a BusyBox ash). Keep one session open; macOS `open()` hangs
after the first open, replug or reboot to recover. Never use `dd skip/seek` on
`/proc/<pid>/mem` (hung the shell); push a small C tool with `pread` instead.
`journalctl -b` shows Evil's stderr (`runevil`). Do not live-ptrace Evil (two
UI freezes: RT threads, SIGCHLD stop on the main thread). The safe way to look
inside the running process is a core dump:
`core_pattern = '|/usr/Evil/Scripts/core.sh %e %p %t'`, `kill -SEGV` the *real*
Evil pid (`ps | grep '[E]vil/Evil$' | grep -v inhibit`), pull the gzip from
`usb_mnt/Coredumps` over serial (`dd | base64`). For a crash loop:
`echo 1 > /proc/sys/kernel/print-fatal-signals`, then `dmesg` for pc/lr.

**Boot constraints.** The registry first failed with a boot-time SEGV when a
fixed-capacity structure overflowed (the ModFac object is `operator new(0x5a4)`
with a 166-word in-object prototype table at `+0x150`). Trace hooks that
print or dereference during early boot hang it; avoid them.

**The picker is name-driven, the rest is id-driven.** Pedal editors are loaded
by name (`qrc:/Pages/ModuleEditors/img/<name minus " 2">.png`, falling back to
`Other.png`); `ModuleTypeN` rig slots store name strings only. That is why the
picker hook adds a name and the registry adds ids, and both must agree.

**Editor / QML.** The UI QML (about 440 plain plus 246 zlib files) and its
compiled cache are embedded in `Evil`'s qrc; only `Assignments/*.qml` exist on
disk. Edits to them are ignored on hardware.

**Dead ends.**

* `patch_namloader.py` (new `ModFac_construct` case 92 via a patched `cmp`):
  type index 92 is not produced by any menu or preset, and the patched
  instruction is on the hot path of every pedal construction.
* `patch_modfac_spy.py`: the `B`/`BL` range is +-32 MB; the near gap needs a
  new `PT_LOAD`, which is known to brick boot on real hardware.
* Live ptrace spy, the `0x209be4` site: zero hits. `ModFac_construct` is slot 7
  of the ModFac vtable (`0x18399c8`), reached only by virtual dispatch, and
  is only called at boot. The vtable-slot spy (`0x18399e4`) worked.
* Adding a pair that reuses an existing type, constant bumping without finding
  the real logic (v3-v5), the metacall logger on the category getter, and the
  `v6` picker hook without string initialization: each taught something but
  none was the fix.
* Single four-instance entry (see above).
* QML edits, renamed parameter names, enum Gain: see NAM_PEDAL_DISPLAY.md.

## Open items

* Drive readout can show the previous model's name until the value moves again;
  needs a UI repaint trigger or a decision to show the number only.
* The patch is not in the shipped GUI pipeline's defaults and not committed.
* Art and captions for the pedal; a generalization to N pairs (the registry
  constants derive from the pair count, but the node-registration hooks still
  assume two pairs, ids 167..170, and seven parameters).
* Debug logging (`[nam_disp]`, `[nam_eng]`) should be trimmed before release.

## History: the first design (static, never applied)

The first attempt (`patch/patch_namloader.py`) cloned Gonkulator's whole
construction shape into a new, independent object graph and gave the effect
factory a new case, type index 92:

* a new engine vtable (byte copy of Gonkulator's with `process()` repointed at
  `patch/trampoline_naml.S`, and slots 14/16 as Input/Output trim setters),
* a new descriptor cell, ctor/clone function and DSPModule-level vtable (byte
  copies with the one pointer each repointed),
* a new `ModFac_construct` case (`patch/case92_stub.S`) reached by replacing
  the `cmp r1,#0x5b` at `0x209be4` with a branch to a stub.

It was validated structurally and under QEMU but never reached: nothing in the
UI produces type 92, and the patched dispatch runs on every pedal
construction. The additive pedals above reach the same goal through the
registry, the picker name list and the node hooks instead, which is what the
firmware itself uses.
