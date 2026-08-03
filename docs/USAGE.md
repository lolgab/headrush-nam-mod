# Using the NAM mod

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

On the **Pedalboard** the on-screen knob labels are relabeled to match (Model /
Inp / Outp); on the **MX5** they keep their original **Drive / Tone / Level**
names but do exactly the same thing.

The pedal's own name stays **Anxiety OD** on both devices — only the knob
labels change (Pedalboard only). A pedal-title rename was tried and works
byte-wise, but real-hardware testing showed it breaks the pedal (the string is
very likely used as an internal type-name lookup key, not just a label), so
it ships disabled.

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

## Recovery

If a flash goes wrong, force firmware-update/recovery mode and reflash the
original, unmodified update file (the installer keeps a copy — see the root
[README](../README.md)):

- **Pedalboard**: hold footswitches **1 and 8** (leftmost, counting left to
  right) while powering on. See
  [this video](https://www.youtube.com/watch?v=6H90kbOCJG8) for a walkthrough.
- **MX5**: hold the **first two** footswitches while powering on.

Tested on real HeadRush Pedalboard and MX5 devices: NAM inference works, and
the device can be safely recovered back to stock firmware via the above.
Proceed at your own risk.
