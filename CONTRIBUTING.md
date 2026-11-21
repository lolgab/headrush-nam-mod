# Contributing

Thanks for helping. This project modifies HeadRush firmware without being
affiliated with inMusic, so it can only stay online if the repository holds
**our own work and nothing of theirs**. Please read this before opening a pull
request.

## What must never be committed

- **Firmware or parts of it**: `Update.img`, the updaters (`.app`, `.exe`,
  `.zip`), the root filesystem, the `Evil` binary, libraries, images or sounds
  taken from the device, in any form (raw, compressed, base64, C arrays).
- **Whole stock files or large parts of them**, even edited. This includes the
  QML pages embedded in `Evil`: a stock page with a few lines changed is still
  their page. Edit stock content **at install time** on the user's own copy
  instead, the way `core/qml_override.c` does: short search strings and our
  own replacement text only.
- **Decompiled or disassembled code**: Ghidra/IDA/radare2 output, pseudo-C,
  listings, in code or in docs.
- **Dumps from a device**: memory, core dumps, `/proc` reads, logs that
  contain firmware content, keys or serial numbers.

## What is fine

- Our own code, scripts, QML and documentation.
- **Short search strings** taken from stock files to find an edit point (a
  line fragment, a property path such as `/Engine/RedirCtrl/CurModule`).
  Keep them as short as they can be while still matching exactly once.
- Addresses, offsets and constants **in source code** where the patcher needs
  them, each checked against the stock bytes before anything is written.
- Docs that explain what our code does and why. Keep firmware internals
  (address maps, struct layouts, internal function names, debugging recipes
  for the device) out of `docs/`; they are not needed to use or build the mod.

## Naming

HeadRush, Pedalboard, MX5 and Gigboard are inMusic's trademarks. Use them only
to say which devices something works with ("NAM mod for HeadRush
Pedalboard"), never as the name of something we make, and don't use their
logos or product photos.

## Pull requests

- Say which devices you tested on, and how (offline, flashed on hardware).
- Builds and patches must keep refusing stock firmware they don't recognize
  rather than guessing.
- By opening a pull request you confirm that the contribution is your own
  work, that you have the right to submit it under the project's license
  (GPLv3, see [LICENSE](LICENSE)), and that it contains none of the material
  listed above.
