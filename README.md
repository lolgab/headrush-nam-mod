# HeadRush Pedalboard, MX5 & Gigboard — Neural Amp Modeler (NAM) mod

Reverse-engineered firmware mod for the HeadRush Pedalboard, MX5, and Gigboard, adding
[Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore)
(NAM, MIT-licensed) neural-network amp-model inference as a pedal.

**Status**: a real, working NAM pedal today, via a hijacked pedal slot,
confirmed on real hardware — see below.

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
2. Run it, pick your model (Pedalboard, MX5, or Gigboard), choose **2 instances**
   or **up to 4 instances** (see "The pedal" below), and click **Install NAM
   Mod** — it downloads the official HeadRush firmware updater and builds a
   patched copy of it in the current directory.
3. Put your device in firmware-update mode. See
   [this video](https://www.youtube.com/watch?v=6H90kbOCJG8) for a
   walkthrough.
4. Run the patched updater it just built:
   - **macOS**: the patched `.app` it wrote — double-click it like the
     official updater.
   - **Windows**: the patched `.exe` it wrote — run it like the official
     updater (unsigned, so SmartScreen may warn about an unrecognized
     publisher — expected for any unofficial build).

Keep the unmodified updater the tool leaves alongside the patched one — you
need it to recover if a flash goes wrong (see
[docs/RECOVERY.md](docs/RECOVERY.md)).

## Loading models

Drop `.nam` files into their own **`/NAM`** folder on the device's USB drive
(sibling to `Impulse Responses`, `Blocks`, `Rigs`, etc — create it yourself via
the File Manager/USB transfer view if it doesn't exist yet). Files are sorted
alphabetically and assigned one per whole percent of the Model knob's sweep,
starting at 0% — name them with a 3-digit index prefix matching the knob
percentage so the number on the file is the number on the knob:

- `000 - First Model.nam`
- `001 - Second Model.nam`
- `002 - Third Model.nam`

With 3 files as above, 0%-2% select them in order; the rest of the knob's
sweep (3%-100%) is silence — no file is assigned there, so the pedal outputs
nothing. This is deliberate: it keeps each file pinned to a fixed knob
position regardless of how many other files are on the drive, instead of the
position drifting every time you add or remove one.

Reboot device after adding/changing files via USB — new models don't show up
till restart.

Keeping that numbering correct by hand gets tedious with a large library, and
a stale/gapped prefix is actively misleading (see above: the number is only a
mnemonic, the real position is always the file's alphabetical rank). The
GUI's **Prepare NAM Library** screen automates this: it finds the device's
`/NAM` folder itself (probing mounted-drive locations for the current OS —
no path to type in, and creating `/NAM` itself if the drive doesn't have one
yet), renumbers everything contiguously (closing any gaps, keeping
already-correct files untouched), and can optionally generate one Anxiety OD
`.block` preset per model — set into that model's `Drive` knob position —
into `Blocks/ANXIETY OD` (created too, if missing) for direct browsing from
the HeadRush's own block/preset picker instead of remembering percentages.
If you installed the **up to 4 instances** mod option, a second checkbox
generates the same set again as `Blocks/ANXIETY OD V2` blocks, for Anxiety OD
V2's hijacked slot. Nothing is renamed or written until you review the
preview and click Apply. A CLI (`nam-library-cli`) is also available for
scripting the same operation (`--find-drives` to list detected drives,
`--v2-blocks` to include the V2 set, or point it at an explicit folder).

## The pedal

On the device, add the **Anxiety OD** pedal.

| Knob | Function |
|---|---|
| **DRIVE** | selects/scans `.nam` model files |
| **TONE** | input trim |
| **LEVEL** | output trim |

This is a *hijack*: Anxiety OD loses its real overdrive function board-wide,
on every instance.

**2 instances vs up to 4**: the stock firmware allows up to 2 Anxiety OD
blocks on a board, so by default that's 2 NAM instances max, and Anxiety OD
**V2** is left untouched, still available with its original sound. Pick
**up to 4 instances** in the installer to also hijack Anxiety OD V2 (also
addable up to twice) the same way, for up to 4 NAM instances total — at the
cost of losing Anxiety OD V2's real sound board-wide too.

## Supported models

| Device | Status |
|---|---|
| HeadRush **Pedalboard** | confirmed on real hardware |
| HeadRush **MX5** | confirmed on real hardware |
| HeadRush **Gigboard** | confirmed on real hardware |

**Only flash the update file built for _your exact model and firmware_** —
flashing another model's file will almost certainly brick the device.

## Gain staging

TONE and LEVEL are both **unity at 50%**, then diverge:

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
- [docs/TECHNICAL.md](docs/TECHNICAL.md) — how the hijack works under the
  hood.
- [docs/BUILDING.md](docs/BUILDING.md) — building `headrush-nam-gui` from
  source.
- [docs/ADDITIVE_PEDAL.md](docs/ADDITIVE_PEDAL.md) — research notes on an
  unshipped, non-hijacking pedal design.

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
