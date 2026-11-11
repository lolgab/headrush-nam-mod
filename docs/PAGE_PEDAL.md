# Footswitch page-turner (USB keyboard)

Optional, **Pedalboard only**. Turns two footswitches into a USB keyboard for
the computer the Pedalboard is plugged into: **Page Down** and **Page Up**, so
you can turn pages in a PDF (sheet music, lyrics) with your foot.

Enable it with the **Footswitch page-turner** checkbox in `headrush-nam-gui`,
or `--page-pedal` on `gui-core-cli`. It is independent of the NAM options and
is installed on top of them.

> **Status: built from static analysis of the firmware, not yet confirmed on
> real hardware.** The pieces that are verified are listed under "What was
> checked"; the physical switch numbering is an assumption you can correct in
> 30 seconds (see "Calibrating").

## Using it

| Footswitch | Sends |
|---|---|
| 5th from the left, **upper** row | Page Down |
| 5th from the left, **lower** row | Page Up |

Plug the Pedalboard into the computer with a USB **data** cable (the
"computer" port). In normal mode it shows up as a keyboard named
*HeadRush Footswitch Keyboard*; in audio-interface mode it is the same audio
device as before plus a keyboard; in USB-drive mode there is no keyboard (the
drive takes over the port) and it comes back when you leave that mode.

**Assign nothing to those two footswitches in the HeadRush UI.** The panel
still reports them to the HeadRush app as usual; if a switch has an effect
assigned, it will do that *and* send the key.

Held down, a switch repeats the way a held keyboard key does.

## Calibrating (which physical switch is which)

The factory firmware numbers the 12 switches FS1-FS12 (panel notes 0-11), but
which of those is the upper or lower row is not stated anywhere in the
firmware, so the defaults are a guess: **FS5 (note 4) = lower row = Page Up,
FS11 (note 10) = upper row = Page Down**.

To check, create a text file named **`PagePedal.conf`** on the Pedalboard's USB
drive (the same top-level folder as `NAM`, `Blocks`, `Rigs` ...) containing:

```
learn=1
```

Reboot the Pedalboard, open any text editor on the computer and press the
footswitches: FS1 types `a`, FS2 types `b` ... FS12 types `l`. Find the two
letters that belong to the 5th-from-left upper and lower switches, then
replace the file's contents with their note numbers (`a`=0, `b`=1, ... `l`=11):

```
page_down_note=10
page_up_note=4
```

| key | default | meaning |
|---|---|---|
| `page_down_note` | `10` | note (FS number - 1) that sends Page Down |
| `page_up_note` | `4` | note that sends Page Up |
| `channel` | `0` | MIDI channel of the panel's notes (0-based) |
| `learn` | `0` | `1` = each switch types a letter instead (calibration) |

The file is re-read every couple of seconds while the daemon runs, but the
USB-drive image is not visible to the device while it is exported to the
computer, so edit it in USB-drive mode and reboot (or just reboot).

## How it works

1. **Where the footswitches come from.** The panel talks to Evil over internal
   MIDI. `/usr/Evil/Assignments/M-Audio_Pass_Through_Assignments.qml` maps
   Note On/Off, channel 0, notes 0-11 to `RawFootswitches/FS1..FS12`. Evil
   reads this through the ALSA sequencer.
2. **`/usr/Evil/pagepedal`** (`patch/pagepedal.c`, plain C, no libraries) opens
   its own ALSA sequencer client and subscribes to the panel's output port.
   ALSA allows any number of subscribers per port, so Evil is not disturbed.
   For the two configured notes it writes standard 8-byte boot-keyboard
   reports to `/dev/hidg0`.
3. **`/usr/Evil/Scripts/pagepedal-usb.sh`** (`patch/pagepedal-usb.sh`) creates a
   configfs gadget `g4` with one `hid` function. It is started in the
   background from the launcher script at boot.
4. **The single USB controller.** The RK3288 has one device controller, which
   the stock audio (`g1`) and USB-drive (`g2`) modes each bind. So the build
   also patches the four stock mode scripts
   (`usb-otg-audio-start/stop.sh`, `setup/remove-mass-storage.sh`): they
   release `g4` before claiming the controller and bring it back afterwards.
   Audio mode additionally gets the keyboard function; **if the controller
   refuses to bind with it** (endpoint budget), the script drops it and binds
   plain audio exactly as stock, so audio mode cannot be broken by this mod.
5. If the USB serial console gadget (`g3`) is also installed, the keyboard is
   added to that gadget instead of fighting it for the controller.

Everything is defensive: the scripts never use `set -e`, always exit 0, and
the launcher entry is backgrounded, so a failure in any of it cannot delay or
block boot. Firmware-update mode is a separate boot path and is unaffected
(see [RECOVERY.md](RECOVERY.md)).

The daemon never writes to the USB-drive image (it is exported to the
computer in USB-drive mode); its log is `/tmp/pagepedal.log` on the device.

## What was checked

- `usb_f_hid` and the sequencer are built into the stock kernel
  (`modules.builtin` of the 2.7 rootfs), and the factory test app already
  drives an HID gadget the same way.
- The footswitch note mapping, from the stock assignments file above.
- The patched stock scripts: anchors match exactly once on the real 2.7 Pedalboard
  scripts, patching is idempotent, and every result passes `sh -n`.
- The gadget script against a mock configfs: bring-up, idempotence, teardown,
  not claiming a busy controller, sharing the console gadget, add/remove of the
  keyboard function.

## What was NOT checked (needs a real Pedalboard)

- That the panel's MIDI really shows up as a kernel ALSA-sequencer client
  (the daemon logs what it subscribes to, and also tries userspace clients
  if it finds no kernel one).
- The upper/lower row numbering (see Calibrating).
- That the audio-interface gadget binds with the extra HID interface on this
  controller (if not, the fallback above makes it behave as stock).
