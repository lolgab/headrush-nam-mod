# Recovery

If a flash goes wrong, force firmware-update/recovery mode and reflash the
original, unmodified update file (the installer keeps a copy — see the root
[README](../README.md)):

- **Pedalboard**: hold footswitches **1 and 8** (leftmost, counting left to
  right) while powering on. See
  [this video](https://www.youtube.com/watch?v=6H90kbOCJG8) for a walkthrough.
- **MX5**: hold the **first two** footswitches while powering on.
- **Gigboard**: no publicly documented footswitch-hold recovery combo exists;
  the stock firmware-update path is UI-driven only (Global Settings →
  Firmware Update on the touchscreen).

Tested on real HeadRush Pedalboard, MX5, and Gigboard devices: NAM inference
works. Pedalboard and MX5 can be safely recovered back to stock firmware via
the footswitch combos above; Gigboard has no known hardware recovery combo,
only the UI-driven update path. Proceed at your own risk.
