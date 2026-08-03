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
2. Run it, pick your model (Pedalboard, MX5, or Gigboard), and click **Install NAM
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

## The pedal

On the device, add the **Anxiety OD** pedal.

| Knob | Function |
|---|---|
| **DRIVE** | selects/scans `.nam` model files |
| **TONE** | input trim |
| **LEVEL** | output trim |

On the **Pedalboard**, the on-screen knob labels are relabeled to match
(Drive → Model, Tone → Inp, Level → Outp). On the **MX5** and **Gigboard**
they keep their stock **Drive / Tone / Level** names but do exactly the same
thing. The pedal's own name stays **Anxiety OD** on every device — that
string is not renamed (tried once, breaks the pedal on real hardware).

This is a *hijack*: Anxiety OD loses its real overdrive function board-wide,
on every instance. That's fine — Anxiety OD **v2** is still available for a
real overdrive.

## Supported models

| Device | Status |
|---|---|
| HeadRush **Pedalboard** | confirmed on real hardware |
| HeadRush **MX5** | confirmed on real hardware |
| HeadRush **Gigboard** | confirmed on real hardware |

**Only flash the update file built for _your exact model and firmware_** —
flashing another model's file will almost certainly brick the device.

## Documentation

- [docs/RECOVERY.md](docs/RECOVERY.md) — what to do if a flash goes wrong.
- [docs/TECHNICAL.md](docs/TECHNICAL.md) — how the hijack works under the
  hood.
- [docs/BUILDING.md](docs/BUILDING.md) — building `headrush-nam-gui` from
  source.
- [docs/ADDITIVE_PEDAL.md](docs/ADDITIVE_PEDAL.md) — research notes on an
  unshipped, non-hijacking pedal design.

## License

The patch scripts and glue code in this repo are licensed under the
[GNU GPLv3](LICENSE). `nam_core/` is MIT-licensed by its own upstream
project (Steven Atkinson).

This project reverse-engineers and modifies HeadRush Pedalboard, MX5, and
Gigboard firmware, which is not affiliated with or endorsed by inMusic/HeadRush.
Use at your own risk; modifying your device's firmware may void its warranty.
