# Custom detail pages (QML)

The detail page of an additive pedal (NAM, NAM TWO, Reverb) shows extra, correct information: the model **number and
name** for NAM, and an **algorithm selector** for the Reverb. Pedalboard 2.7 only; the patcher refuses on any
mismatch. Everything here was verified on hardware.

## Why the old "patch the embedded QML" attempt failed

`Evil` keeps every QML file twice: the source in its Qt resources and a `qmlcachegen` unit that wins at load time.
Editing only the source changes nothing. The units are found through a hash keyed by static UTF-16 `QString`s
(`"/Pages/BlockDetails.qml"`, 535 of them; the registry constructor around code `0x5d0f48` inserts `(&QStringData,
&unit)` pairs). **Changing the last character of a key** (`.qml` to `.qmx`) makes that lookup miss and Qt compiles the
embedded source instead. Not enough: `QML_DISABLE_DISK_CACHE`, or corrupting the units' version field (compiled-in
units are not header-checked).

## What the patcher does (`core/qml_override.c`)

1. Inflates the stock `BlockDetails.qml` entry (zlib, `qCompress` framing: `[be32 length][be32 ulen][stream]`),
   replaces the line `Loader { source: <ui file> + ".qml" }` so that the UI file name containing `MorningGlory` (the
   editor slot the additive pedals share, which has no editor of its own) loads
   `file:///usr/Evil/Resources/pedal_ui.qml`, and recompresses it into the **same slot** (the resources are not
   rebuilt; comment and blank lines are dropped if it does not fit, the tail is zero padded).
2. Breaks the unit key of `/Pages/BlockDetails.qml`.
3. Injects the pages into `/usr/Evil/Resources/`: `patch/ui/pedal_ui.qml` (dispatcher by pedal name), `nam_ui.qml` and, with
   `--reverb-pedal`, `reverb_ui.qml`, plus `nam_fix.qml`, `pedal_art.qml` and `rig_block.qml` (below). The pages can be edited without any size limit; they only use
   `Evil.getProperty(...).translator` (`unnormalized`, `index`, `string`).

`tools/qml_patch.py` does the same for the dev loop, on a live device, from RAM (reboot reverts): see
[DEV_LOOP.md](DEV_LOOP.md). Pitfalls: a QML component is cached by URL (add `?<timestamp>` to reload a file without
restarting Evil), this Qt has no `XMLHttpRequest` (data goes in through a `Loader` of a QML file), and `console.warn`
(not `log`) reaches the journal.

## NAM: model number and name

The stock Drive readout formats its text when the value changes, using the name the hook had written by then, and keeps
it until the next change: a stale number/name pair ("1 AC30" where AC30 is model 2) that stays wrong. `nam_ui.qml`
derives number and name from the same live value (`/tmp/nam_models.qml`, a tiny QML list the hook rewrites whenever
`drive_names()` changes, with the same label rules as the readout) and, while open, re-assigns the value when the
stock text disagrees (every 40 ms), which refreshes the readouts next to it. Only while the page is open: on other
screens the stock text can still lag.

## What else is edited (all in `core/qml_override.c`, same method)

| File (unit key) | Edit |
|---|---|
| `Pages/BlockDetails.qml` | the editor Loader (above), and the fallback picture (`Other.png`) becomes a Loader of `pedal_art.qml` |
| `Groups/EncoderParamInfo.qml` | the tiles' captions use the real names (a plain, uncompressed entry: replaced whole by `patch/ui/EncoderParamInfo.qml`, which has to fit the stock source's 2426 bytes) and, for NAM pedals, a Loader of `nam_fix.qml` |
| `Pages/EncoderField.qml` | the tile count drops by the switches the pages replace (NAM: Hi-Cut, Reverb: Gain and Hi-Cut) |
| `Groups/SignalChainModuleStomps/ActiveStomp.qml` | the Reverb's stock stomp picture is recoloured blue (`Colorize` hue) and a Loader of `rig_block.qml` goes in right after the stock LED picture |

* `pedal_art.qml`: the pedal drawn on the detail page (288 x 457): body, LED, footswitch, three knobs that turn with the live
  values, "NAM" and the loaded model, or "REVERB" and the algorithm. Everything else keeps the stock fallback picture.
* `rig_block.qml`: the Rig page block (183 x 285) is the stock stomp picture with, on top, three small knobs and the model
  (NAM) or algorithm (Reverb) under the name. It does not replace the stock picture: the block is drawn at half opacity
  when the pedal is bypassed and a covering body lets the stock one show through; and a body that is wider than the stock
  one lets the neighbouring empty slot's box draw over it.
* `nam_fix.qml`: loaded by every tile of a NAM pedal on every page (the Rig page included): the same refresh of the stock
  model readout as `nam_ui.qml`.

## Real parameter names

The stock tiles on the right of the page keep the engine's names (DRIVE / LEVEL / TONE: the additive pedals are Glorious
Drive clones, and the editor looks parameters up by those names). Our pages show the real ones: NAM: MODEL (Drive),
INPUT (Level), OUTPUT (Tone), CALIBRATION (the Gain switch); Reverb: DECAY (Drive), MIX (Level), DAMPING (Tone).
The tile captions are edited too (`EncoderParamInfo.qml`, table above): the tiles show MODEL / INPUT / OUTPUT / CALIB for NAM
(the Gain tile's Low / High read OFF / ON) and DECAY / MIX / DAMPING for Reverb.

## Reverb: algorithm selector

`reverb_ui.qml` shows the algorithm and has PLATE / HALL / ROOM / SPRING buttons (Gain and Hi-Cut tiles are hidden). They set two switches the stock pedal
already saves with the preset: **Hi-Cut** (engine byte `0x57f`, On by default) and **Gain** (`0x57c`, Low by default):
Hi-Cut On: Gain Low = Plate, High = Hall; Hi-Cut Off: Gain Low = Room, High = Spring (`default_reverb_algo()` in
`patch/nam_hook.cpp`).
