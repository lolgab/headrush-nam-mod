# Optional mods

Besides NAM, the installer can apply these mods. Pick any combination in the
GUI, or with `gui-core-cli --mods` (see `docs/BUILDING.md`). The NAM mod is
optional too.

| Mod | CLI key | Models | Requires |
|---|---|---|---|
| [USB serial console](usb_console/README.md): root shell + spare serial channel over USB | `usb-console` | MX5 | — |
| [Remote screen](remote_screen/README.md): view and operate the screen from a computer | `remote-screen` | MX5 | `usb-console` |
| [Tuner tempo edit](tuner_tempo/README.md): tap the tuner's tempo box to change BPM | `tuner-tempo` | MX5 | — |

All three were developed and confirmed on a real MX5 running firmware 2.7.
The installer refuses to apply them to any other model; the sections below
cover what porting each one would take.

## How it fits together

Every mod lives in this repo and is compiled into the installer — there is no
plugin format. A mod is:

- an entry in `NAM_MODS` (`core/mods.c`): CLI key, GUI label, the models it
  has been **verified** on, and an optional required mod;
- an apply step: NAM's is in `core/patch_pipeline.c`, the others are in
  `core/device_mods.c`;
- payload files it installs, under `mods/<mod>/`, embedded into the binary at
  build time (`nam_embed_payload()` in `CMakeLists.txt`). Anything that has to
  be cross-compiled for the device goes through
  `scripts/build_blobs_native.sh` into `blobs/`, like the NAM libraries.

Every build starts from the freshly downloaded stock image, so uninstalling a
mod just means reflashing without it.

To add a mod: add a `NamModId` and a `NAM_MODS` entry, write its apply
function, call it from `nam_patch_pipeline()`, and embed its payloads. Follow
the existing refuse-rather-than-guess pattern: check the model, check that every
byte you overwrite or anchor you insert at is what you expect, and fail with a
clear message otherwise. A wrong guess can leave a device stuck on the boot
splash.

## Porting to Pedalboard and Gigboard

What follows comes from comparing the stock 2.7 firmware of all three models
(rootfs, kernel, device tree, `Evil` binary). None of it has been tried on a
Pedalboard or Gigboard. To enable a mod for a model after testing it, add the
model to the mod's `models` list in `core/mods.c` and relax the model check in
its apply function in `core/device_mods.c`.

### USB serial console — probably works unchanged

Matches the MX5 in every way that matters:

- The kernels (`5.10.96-inmusic-2022-02-02-rt60` on all three) all have the
  CDC-ACM gadget function, `ttyGS`, and libcomposite built in.
- The stock USB mode scripts `usb-otg-audio-start.sh` and
  `setup-mass-storage.sh` differ from the MX5's only in their VID/PID/product
  strings (and the Pedalboard's mass-storage gadget uses a fixed serial
  number). Both anchors the mod inserts its teardown call at are present, and
  all three bind the same UDC, `ff580000.usb`.
- The console's PID `0x5017` isn't used by any stock gadget on any model
  (audio is `0x4016`/`0x4018`/`0x401A` for Pedalboard/Gigboard/MX5,
  mass storage is `0x5016` on all three).
- `/etc/systemd/system/multi-user.target.wants/` exists on all three.

Changes needed: probably none, beyond the product string in
`setup-usb-console.sh`, which says "HeadRush MX5 Console" (cosmetic).

To test: flash, check that the two serial ports appear and the shell works, then
enter and leave **audio interface mode** and **USB drive mode**. Those mode
switches are what the teardown patch exists for. Also check that power-off
doesn't hang.

### Remote screen — needs viewer changes, encoder work, and testing

- **Rotation differs.** The device tree says the MX5's panel is rotated 90°
  (`inmusic,panel-rotation = <0x5a>`), and Pedalboard/Gigboard are rotated 270°
  (`<0x10e>`). The server reads the framebuffer size at runtime, but both viewers
  hard-code the MX5's counter-clockwise rotation and its inverse for clicks. On
  a Pedalboard or Gigboard the picture would be upside down and clicks would land
  in the wrong place. Fix: have the server report the rotation (e.g. read
  `/proc/device-tree/.../inmusic,panel-rotation`, or take a flag from the unit
  file) in its stream header, and have the viewers use it.
- **Encoder emulation is MX5-specific.** The server becomes a MIDI source on
  the ALSA port `Midi::In::HG04 Control Surface MIDI 1` and sends CC 3 / Note 4,
  which the MX5's assignment file maps to `/Engine/PushEncoderCtrl/...`.
  Pedalboard and Gigboard use an `M-Audio Internal` control surface instead,
  with several `/Engine/RedirCtrl/EncoderN` knobs. Without changes the server
  logs that the port is missing and runs with the encoder disabled. Screen and
  touch would still work.
- **Touch** goes through a virtual uinput touchscreen in framebuffer
  coordinates. The hardware touch controllers differ (Sitronix ST1633 on the
  MX5; Ilitek ILI2116/2117 or Sitronix ST1727 on the others), but that doesn't
  affect a virtual device. Whether Qt maps virtual-touch coordinates the same
  way under the 270° rotation needs testing.
- **Things that match:** all three are quad-core RK3288s, so the unit's
  `taskset -c 3` is valid (the per-core load was only measured on an MX5).
  `runevil` and the `evil` launcher are byte-identical across models. All
  three `Evil` binaries have the same hardcoded `/dev/input/event2-4`
  keyboard blacklist that the server works around. The capture method needs
  Qt to render through the fbdev-backed `eglfs_mali` backend; confirm that
  `/dev/fb0` holds the live screen.

### Tuner tempo edit — needs new per-model payloads

This mod overwrites Tuner.qml's embedded source with an edited copy of exactly
the same length, so each model needs its own hand-made payload plus verified
offsets in `TUNER_TEMPO_TARGETS` (`core/device_mods.c`).

- **Gigboard:** the tuner has the same read-only tempo box, but in a different
  layout (`TunerBPMInfo { x: 790; y: 472; ... }` — a bigger screen), and the
  source region is 3442 bytes at file offset `0x185db1b` in 2.7's `Evil`. The
  compiled unit is probably the `qv4cdata` at `0x2abc9e0`, found the same way
  as the MX5's (the last unit magic before the UTF-16 literal
  `focusTunerRef`). Write a same-length `Tuner_gigboard.qml` with the
  `MouseArea` wrapper scaled to that layout, and add a table entry.
- **Pedalboard:** the tuner page has **no tempo display at all**, so there is
  nothing to wrap. Porting means designing a new element to fit into the
  existing source's byte budget (compiled-unit candidate: `0x2bf3d40`).

The technique (QML source fallback) is in `tuner_tempo/README.md`. It was proven
on the MX5. All three models' compiled QML units carry the same header (unit
format `0x29`, built with Qt `0x050f02` = 5.15.2), so the trick should carry
over.
