# NAM pedal display: dB faders and the Drive model readout

Notes from reverse engineering how the HeadRush Pedalboard 2.7 UI shows a
pedal's knob values, and how the additive NAM pedals (NAM A / NAM B, two
instances each) use it. Everything here was learned on real hardware
(firmware 2.7 `Evil`, ARM32, non-PIE Qt/QML) unless marked *unverified*.

Code: `patch/nam_hook.cpp` (the "display" section at the end of the file,
`display_watcher()` and friends), `patch/nam_preload.cpp`,
`core/spy_patch.c` (registration hooks), `patch/engine_stub.S`.

## What the user sees

| Knob  | Engine meaning        | Display                                   |
|-------|-----------------------|-------------------------------------------|
| Drive | model selection       | file number `1..N`, plus the file name    |
| Level | output volume         | dB, same scale as the FX-Loop *Return*    |
| Tone  | input volume          | dB, same scale as the FX-Loop *Send*      |

The captions (DRIVE / LEVEL / TONE) cannot be changed; see "Dead ends".

## How the UI gets its text

Every pedal parameter is a *MixerCtrl property*. Per pedal instance there are
two twin sets of objects, one under `/Engine/MixerCtrl/<pedal>/<param>` and one
under `/Engine/MixerCtrl2/<pedal>/<param>`.

```
prop            vtable 0x2b96734 (all MixerCtrl props)
  +0x2c  cb     control block; its vtable tells float props (0x2b99d40)
                from enum props (0x2b98fb4)
  +0x30  std::string { ptr, len }   the path, e.g. "/Engine/MixerCtrl/NAM A/Drive"
cb  +0x20  -> desc  (translator / descriptor)
desc (0x58 bytes for float params)
  +0x00  use count (0 idle, ~6 while the editor holds it)
  +0x18  current value, normalized 0..1          (+0x20 holds a copy)
  +0x1c  default (0.5)                            (+0x24 holds a copy)
  +0x28  min      +0x2c  max      +0x30  step
  +0x38  inline format string, 32 bytes, e.g. "%.0f %%", "%.1f dB"
```

The UI turns the normalized value into the text it shows as
`min + v * (max - min)` formatted with the string at `+0x38`. Nothing else
is consulted, so changing `min`, `max`, `step` and the string is enough to
re-skin a fader. The FX-Loop *Return* / *Send* descs are the stock dB faders
(min -60, max +12, step 0.1, `"%.1f dB"`, default 0.8333). The NAM pedals
are clones of Glorious Drive and start with percent descs (0..100,
`"%.0f %%"`).

Important: **the descs are not fixed objects.**

* Every knob move leaves the prop pointing at a *freshly allocated* desc (the
  log shows a new address on almost every move, with the fmt of the previous
  state copied in).
* The UI formats the text once, when the value changes, from that new desc.
  It does not re-read the desc afterwards. A new text shows up only when the
  value changes again or the edit page is reopened.

## How the hook patches them

`nam_eng_init()` (called from the registration hook, see "Registration")
starts `display_watcher()` once. It:

1. **Sweeps the heap** for the 56 path strings (28 props x two mixers):
   one pass with `memmem("/Engine/MixerCtrl")` over the heap regions, then a
   scan of a +/-32 KB window around each hit for the pointer to the text
   (the prop lives next to its path text). Cold, the first sweep takes about
   4 s (the boot model scan competes for the CPU); later sweeps about 160 ms.
   The whole-heap pointer scan this replaced took 4-8 s every time.
2. **Level / Tone**: copies `desc+0x18..0x58` of the FX-Loop Return / Send
   descs over each NAM Level / Tone desc (`clone_desc`). Only when
   `desc_sane()` passes (use count <= 4096, defaults in [0,1], a known
   range, printable NUL-terminated fmt), because a freshly added pedal's
   half-built or freed memory also reads as valid zeros and writing there
   crashed the engine.
3. **Drive**: `set_desc_index()` sets min 1, max N, step 1 and the format
   `"%.0f <file name>"` (file name: `.nam` and a `NNN - ` prefix stripped,
   max 28 chars, `%` escaped). The format keeps the leading `%.0f `; with a
   name-only format the knob stopped responding.
4. **Between sweeps** a 2 ms loop follows `prop -> cb -> desc` for each known
   Drive prop (`prop_desc()`), reads the value at `desc+0x18`, and re-patches
   as soon as the value or the file count changes. It must follow the prop to
   its *current* desc each tick: patching only the desc found at the last
   sweep lags by up to a second because the desc address changes per move.
5. Sets `disp_db` on every engine instance when all NAM Level and Tone descs
   read in dB, so the engine uses the matching gain curve
   (`trim_gain_fx`: dB = -60 + 72x, silent at <= 0.4%) instead of the
   percent curve.

### File list at boot

The validated model cache (`g_cached_models`) is only ready about 25 s after
boot (every file is test-built). `drive_names()` falls back to a plain
directory listing sorted the same way until then; otherwise the pedal that
comes up with a preset showed the default percent display for 25 s.

### One index rule

The number the editor shows is `round(min + v * (max - min))`, i.e.
`1 + round(v * (N - 1))`. The name and the engine must use the same rule or
a slightly moved knob shows number k but plays or names model k+-1.
`zone = clamp(lround(v * (N - 1)), 0, N - 1)` is now used by the name
(`set_desc_index`) and by the engine (the additive-pedal branch in
`process()`); it replaced `floor(v * N)`.

### Pairing engines with pedal instances

The slow, fragile part of the first versions. The module only knows the pedal
*type* ("NAM A"), not which instance it is; instances were paired by engine
creation order, and a module lookup by heap sweep took seconds and failed for
new pedals ("adding pedal n updated pedal n-1"). The props keep their current
value at `desc+0x18`, so none of it is needed any more: each Drive prop is
patched from its own value, independent of the engines.

## Registration (additive pedals)

`core/spy_patch.c` patches the node-registration blob (616 bytes, hooks A-D)
and the engine class. Relevant to display:

* The hook calls `*init_slot(triples, count, builder)` with `(storage, name,
  path)` triples; the hook library exports `nam_eng_init` for that slot,
  filled from env vars by `libnam_preload.so`
  (`NAM_HOOK_SLOT_ENG_INIT_ADDR`, `NAM_ENG_TAB_ADDR`).
* The parameter-name table (Glorious Drive's: Drive, Level, Tone, Gain,
  Hi-Cut, On, Colour at vaddr 0x33ca244) is also used as a lookup key, which
  is why renaming it broke the display (below).

## Dead ends (do not retry)

* **Changing the captions DRIVE / LEVEL / TONE.** The UI QML (and its compiled
  cache) is embedded in `Evil`'s qrc. Patching the embedded QML is ignored on
  hardware. Renaming the entries in the parameter-name table (experiment
  `--rename-params`, build v29: "Model" / "OutVol" / "InVol", registration
  blob copies the 7-word table into the RW tail and repoints it) keeps the old
  captions and **removes the numbers**: the editor looks parameters up by
  their original names. Same failure as renaming the pedal title.
* **Reusing the Gain LOW/HIGH switch for the model list.** `Gain` is an *enum*
  prop (cb vtable 0x2b98fb4). Its desc has 160 bytes with 32-byte name slots
  at `+0x1c`, `+0x3c`, `+0x5c`, `+0x7c`. The only builder (`0x5a74b0`, 31
  callers) fills exactly two names and leaves "Unassigned" in the third. The
  names are static, so an enum would have no formatting lag, but it holds at
  most 2 (maybe 4) names and the widget is a switch. A model library of 9+
  files does not fit. Making Drive an enum would need a bigger enum
  desc + callback, an index-valued engine knob and a UI that accepts more than
  two states (*unverified*); not attempted.
* **Hooking the text formatter.** Probing `vsnprintf` / `snprintf` through the
  preload (the hook lib is `RTLD_GLOBAL`) never fired for knob text: Qt
  formats the float itself (statically linked `QString::asprintf`,
  `doubleToString`), no libc call to interpose. The `sprintf` callers in
  `Evil` are unrelated (ALSA names, JSON escaping).
* **Nudging the value to make the UI repaint.** Briefly writing `v + 1e-4` to
  `desc+0x18/+0x20` and restoring it did not repaint the text.
* **Fixed-width name column.** The format string is 31 characters inline; a
  name longer than ~24 chars is truncated and the editor shrinks long text.

## Known limits

* The UI formats once per value change from the desc it holds at that moment.
  The 2 ms poll patches the new desc right after it appears, but if the UI
  formats first, the text shows the previous file until the value moves again
  or the page is reopened. A hard guarantee needs a repaint trigger inside the
  UI (not found). *The final polling version (follows `prop -> current desc`)
  was deployed but its consistency was not yet confirmed by the user when
  this was written.*
* The boot sweep takes about 4 s; the pedal shows percent for that long after
  a restart.
* The file count at boot comes from a directory listing and can differ by the
  number of corrupt files from the validated count that follows ~25 s later.
* Hot-patching writes into the engine's heap from a background thread, guarded
  by `desc_sane()` and a re-check of `prop -> desc`; this is best effort, not
  synchronized with the UI thread.

## Hot deploy without flashing

The root filesystem is read-only; a serial root shell
(`/dev/cu.usbmodem0000000000001`, 115200) can bind-mount a new library over
`/usr/Evil/libnam_hook.so`:

1. `base64`/`gzip` the library, send it in 1000-byte `printf >>` chunks to
   `/tmp/n.b64`, decode to `/tmp/lib_<md5>.so`.
2. `umount -l /usr/Evil/libnam_hook.so` (twice; stacked mounts served a stale
   file), then `mount --bind /tmp/lib_<md5>.so /usr/Evil/libnam_hook.so`.
3. Kill the running `/usr/Evil/Evil` (not the `systemd-inhibit` wrapper); the
   launcher restarts it with the new library.
4. `journalctl -b | grep nam_disp` shows the display log.

Not persistent: a reboot returns to the flashed library. Use unique `/tmp`
names per build; reusing a name bind-mounts the stale file.

Log lines (`[nam_disp]`): `sweep: ... N ms`, `<pedal> Level|Tone desc ... now
dB`, `drive desc <addr> -> '<fmt>' (was '<fmt>')`, `model k: <file>` (index to
file mapping, printed when the count changes).

## Build and flash

`./scripts/build_blobs.sh` (Docker, about 5 minutes; Docker Desktop must be
running) builds `blobs/libnam_hook.so`, `libnam_preload.so` and the
trampolines. For a flashable image:
`cd build && make gui-core-cli`, then
`./build/gui-core-cli <base Update.img> out.img --model pedalboard --additive`,
copy the image into a `ditto --noextattr --norsrc --noqtn` copy of the base
`.app` and `codesign --force --deep --sign -` it (a plain `cp -R` breaks the
signature and the app reports "damaged").

Build history of the display work: v26 knobs, v27/v28 dB + names (flashable
fallback), v29 renamed parameter names (failed, do not flash). The model-number
Drive display and the 2 ms follow-the-desc poll exist only as hot-deployed
libraries until the next image is built.
