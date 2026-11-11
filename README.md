# HeadRush Pedalboard, MX5 & Gigboard — Neural Amp Modeler (NAM) mod

Reverse-engineered firmware mod for the HeadRush Pedalboard, MX5, and Gigboard, adding
[Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore)
(NAM, MIT-licensed) neural-network amp-model inference as a pedal.

**Status**: on the Pedalboard, two real **NAM** pedals (**NAM** and **NAM 2**)
are added to the Distortion list, next to the stock pedals. On every model,
the older design is still available as an opt-in: it makes **Anxiety OD** run
NAM, so existing pedalboards that use it keep working. See below.

## Install

1. Download `headrush-nam-gui` for your OS from the
   [GitHub Releases page](https://github.com/lolgab/headrush-nam-mod/releases).
   **Windows**: it's a `.zip` — unzip it and keep the `.exe` together with
   the `.dll` files next to it; they're required, not optional.
   **macOS**: it dynamically links SDL2 (and, on Intel Macs, curl and xz
   too) from Homebrew — install whichever your Mac needs before running it:
   - **Apple Silicon**: `brew install sdl2`
   - **Intel**: `brew install curl xz sdl2`

   Without these, `headrush-nam-gui` fails to launch with a
   `Library not loaded` error.
2. Run it, pick your model (Pedalboard, MX5, or Gigboard), and click **Install
   NAM Mod** — it downloads the official HeadRush firmware updater and builds
   a patched copy of it in the current directory. On the Pedalboard it adds the
   NAM pedals by default; tick **Also make Anxiety OD run NAM** if you have
   pedalboards that use Anxiety OD as the NAM engine and want to keep them
   (see "The pedals" below). MX5 and Gigboard don't have the NAM pedals yet and
   always use the Anxiety OD design.
3. Put your device in firmware-update mode. See
   [this video](https://www.youtube.com/watch?v=6H90kbOCJG8) for a
   walkthrough.
4. Run the patched updater it just built:
   - **macOS**: the patched `.app` it wrote — double-click it like the
     official updater.
   - **Windows**: the patched `.exe` it wrote — run it like the official
     updater (unsigned, so SmartScreen may warn about an unrecognized
     publisher — expected for any unofficial build).

The installer keeps the official updater it downloaded (about 60 MB) in your
user cache folder (macOS `~/Library/Caches/headrush-nam-mod`, Windows
`%LOCALAPPDATA%\headrush-nam-mod\cache`, Linux `~/.cache/headrush-nam-mod`) and
reuses it on the next run instead of downloading again. Its SHA-256 is recorded
on the first download and checked every time; a file that no longer matches
(truncated, corrupted, edited) is discarded and downloaded afresh. Delete the
folder, or set `NAM_NO_CACHE=1`, to force a fresh download.

Keep the unmodified updater the tool leaves alongside the patched one — you
need it to recover if a flash goes wrong (see
[docs/RECOVERY.md](docs/RECOVERY.md)).

## Loading models

Drop `.nam` files into their own **`/NAM`** folder on the device's USB drive
(sibling to `Impulse Responses`, `Blocks`, `Rigs`, etc — create it yourself via
the File Manager/USB transfer view if it doesn't exist yet). Files are sorted
alphabetically; a file's position in that order is its model number. Name them
with a 3-digit index prefix so the order is explicit:

- `000 - First Model.nam`
- `001 - Second Model.nam`
- `002 - Third Model.nam`

**NAM pedals**: the **DRIVE** knob shows the model number `1..N` (N = number of
files) and the file name under it, so `000 - First Model.nam` is model 1. Every
file is reachable, and the knob always lands on a real file.

**Anxiety OD option**: files are assigned one per whole percent of the Drive
knob's sweep, starting at 0% — so the number in the file name is the number on
the knob. With 3 files as above, 0%-2% select them in order; the rest of the
sweep (3%-100%) is silence — no file is assigned there, so the pedal outputs
nothing. This is deliberate: it keeps each file pinned to a fixed knob position
regardless of how many other files are on the drive.

Reboot device after adding/changing files via USB — new models don't show up
till restart.

Keeping that numbering correct by hand gets tedious with a large library, and
a stale/gapped prefix is actively misleading (the number is only a mnemonic,
the real position is always the file's alphabetical rank). The
GUI's **Prepare NAM Library** screen automates this: it finds the device's
`/NAM` folder itself (probing mounted-drive locations for the current OS —
no path to type in, and creating `/NAM` itself if the drive doesn't have one
yet), renumbers everything contiguously (closing any gaps, keeping
already-correct files untouched), and can optionally generate one Anxiety OD
`.block` preset per model — set into that model's `Drive` knob position —
into `Blocks/ANXIETY OD` (created too, if missing) for direct browsing from
the HeadRush's own block/preset picker instead of remembering percentages.
That is only useful with the **Anxiety OD option**; the NAM pedals don't need
it. If you installed the **up to 4** Anxiety OD option, a second checkbox
generates the same set again as `Blocks/ANXIETY OD V2` blocks, for Anxiety OD
V2's hijacked slot. Nothing is renamed or written until you review the
preview and click Apply. A CLI (`nam-library-cli`) is also available for
scripting the same operation (`--find-drives` to list detected drives,
`--v2-blocks` to include the V2 set, or point it at an explicit folder).

## The pedals

### NAM and NAM 2 (Pedalboard)

On the device, add **NAM** from the **Distortion** list, and **NAM 2** by
adding it a second time (like any other pedal and its "2" twin). Nothing stock
is replaced, so every other pedal and every saved pedalboard behaves as before.

| Knob | Function | Display |
|---|---|---|
| **DRIVE** | selects the `.nam` model | model number `1..N` and the file name |
| **TONE** | input volume | dB, same scale as the FX Loop SEND |
| **LEVEL** | output volume | dB, same scale as the FX Loop RETURN |

The knob captions can't be changed (they are looked up by name in the
firmware); they stay DRIVE / TONE / LEVEL. Known quirks: the pedal shows
percentages for a few seconds after the device boots; the name under the number
can lag behind a fast knob turn until you stop or reopen the page. See
[docs/NAM_PEDAL_DISPLAY.md](docs/NAM_PEDAL_DISPLAY.md).

### Anxiety OD option (all models, opt-in)

Tick **Also make Anxiety OD run NAM** in the installer (Pedalboard; MX5 and
Gigboard always use it) if you have pedalboards that use Anxiety OD as the
NAM engine and want to keep them. On the device, add the **Anxiety OD** pedal.

| Knob | Function |
|---|---|
| **DRIVE** | selects/scans `.nam` model files |
| **TONE** | input trim |
| **LEVEL** | output trim |

This is a *hijack*: Anxiety OD loses its real overdrive function board-wide,
on every instance. The two options can be installed together on the Pedalboard.

**2 instances vs up to 4**: the stock firmware allows up to 2 Anxiety OD
blocks on a board, so by default that's 2 NAM instances max, and Anxiety OD
**V2** is left untouched, still available with its original sound. Pick
**up to 4 instances** to also hijack Anxiety OD V2 (also addable up to twice)
the same way, for up to 4 hijacked NAM instances total — at the cost of losing
Anxiety OD V2's real sound board-wide too.

## Footswitch page-turner (optional, Pedalboard only)

Tick **Footswitch page-turner** in the installer (or pass `--page-pedal` to
the CLI) and the Pedalboard also acts as a USB keyboard for the computer it's
plugged into: the 5th footswitch from the left, **upper** row = **Page Down**,
**lower** row = **Page Up**, for turning pages in a PDF. Leave those two
switches unassigned in the HeadRush UI. The upper/lower numbering is a guess
until confirmed on hardware, and there's a built-in calibration mode — see
[docs/PAGE_PEDAL.md](docs/PAGE_PEDAL.md).

## Supported models

| Device | Status |
|---|---|
| HeadRush **Pedalboard** | confirmed on real hardware |
| HeadRush **MX5** | confirmed on real hardware |
| HeadRush **Gigboard** | confirmed on real hardware |

**Only flash the update file built for _your exact model and firmware_** —
flashing another model's file will almost certainly brick the device.

## Gain staging

**NAM pedals**: TONE (input) and LEVEL (output) are dB faders from -60 dB to
+12 dB (0 dB = unity), the same scale and display as the FX Loop SEND/RETURN.
The bottom of the travel is silence.

**Anxiety OD option**: TONE and LEVEL are both **unity at 50%**, then diverge:

- **0%-50%**: linear amplitude fade from true silence (0%) up to unity/0dB
  (50%) — a plain volume taper, not a dB curve.
- **50%-100%**: dB boost, linear in dB, 0dB (50%) up to **+12dB (100%)**.

| Knob % | Gain |
|---|---|
| 0% | silence |
| 10% | -14.0 dB |
| 20% | -8.0 dB |
| 30% | -4.4 dB |
| 40% | -1.9 dB |
| 50% | 0 dB (unity) |
| 60% | +2.4 dB |
| 70% | +4.8 dB |
| 80% | +7.2 dB |
| 90% | +9.6 dB |
| 100% | +12.0 dB |

## Documentation

- [docs/RECOVERY.md](docs/RECOVERY.md) — what to do if a flash goes wrong.
- [docs/TECHNICAL.md](docs/TECHNICAL.md) — how the Anxiety OD hijack works
  under the hood.
- [docs/ADDITIVE_PEDAL.md](docs/ADDITIVE_PEDAL.md) — how the NAM pedals are
  added to the firmware.
- [docs/NAM_PEDAL_DISPLAY.md](docs/NAM_PEDAL_DISPLAY.md) — the dB faders and
  the model readout, and what doesn't work.
- [docs/BUILDING.md](docs/BUILDING.md) — building `headrush-nam-gui` from
  source.

## Disclaimer

This software is provided "as is", without warranty of any kind, express or
implied, including but not limited to the warranties of merchantability,
fitness for a particular purpose, and noninfringement. In no event shall the
authors or contributors be liable for any claim, damages, or other liability
arising from, out of, or in connection with the software or the use or other
dealings in the software.

Flashing custom firmware carries an inherent risk of bricking your device.
By using this software, you accept full responsibility for any damage to
your hardware, including but not limited to a failed flash, data loss, or
voided manufacturer warranty. Proceed at your own risk.

## License

The patch scripts and glue code in this repo are licensed under the
[GNU GPLv3](LICENSE). `nam_core/` is MIT-licensed by its own upstream
project (Steven Atkinson). Everything else statically linked, vendored, or
(on Windows only) redistributed as a DLL -- Eigen, miniz, Nuklear, sha1,
SDL2, curl, xz, OpenSSL, 7-Zip, etc. -- is under its own permissive
license; full texts and per-release-artifact scope are in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), which ships inside
every release download too.

This project reverse-engineers and modifies HeadRush Pedalboard, MX5, and
Gigboard firmware, which is not affiliated with or endorsed by inMusic/HeadRush.
Use at your own risk; modifying your device's firmware may void its warranty.
