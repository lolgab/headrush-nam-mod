# HeadRush Pedalboard, MX5 & Gigboard — Neural Amp Modeler (NAM) mod

Reverse-engineered firmware mod for the HeadRush Pedalboard, MX5, and Gigboard, adding
[Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore)
(NAM, MIT-licensed) neural-network amp-model inference as a pedal.

**Status**: a real, working NAM pedal today, via a hijacked pedal slot,
confirmed on real hardware — see [docs/USAGE.md](docs/USAGE.md).

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
[docs/USAGE.md](docs/USAGE.md#recovery)).

## Documentation

- [docs/USAGE.md](docs/USAGE.md) — which pedal to add, what the knobs do,
  how to name/place `.nam` model files, supported models, recovery mode.
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
