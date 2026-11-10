# MX5 remote screen

Mirror the HeadRush MX5's display on a PC over USB, and operate it with the
mouse. **Confirmed working on real MX5 hardware** (firmware 2.7).

Requires the second USB serial channel from [`../mx5_usb_console`](../mx5_usb_console)
(`ttyGS1`), so flash that first.

```
                MX5                                    PC
   /dev/fb0 ──► mx5_screen_server ──► ttyGS1 ═══USB═══► mx5_viewer.py ──► window
   /dev/uinput ◄──────────────────────  ttyGS1 ◄═══════════════════════ mouse
```

## Running it

### Installed (recommended)

`./build_install_screen.py` bakes the server into the firmware and enables it at
boot, so the screen share is live as soon as the device is up — no login, and no
re-pasting the binary after every reboot. Chain it after the gadget mod:

```sh
../mx5_usb_console/build_usb_console.py stock.img a.img
./build.sh                                            # produce the ARM binary
./build_install_screen.py                a.img     final.img
```

Then open a viewer. Two are provided; they speak the same protocol and are
interchangeable.

**Browser (no dependencies)** — open `mx5_viewer.html` in Chrome or Edge, click
**Connect**, and pick the *second* MX5 serial port (the data channel; the first
is the shell). Uses the Web Serial API, so there is nothing to install.

You only pick it once. `requestPort()` needs a user gesture, but `getPorts()`
returns already-granted ports without one, so later visits connect on their own —
and the viewer also reconnects when the MX5 is plugged in or finishes rebooting.
Because both ACM ports come from one composite gadget they share a VID/PID and
`getInfo()` can't distinguish them, so the viewer probes: it asks each granted
port for a keyframe and keeps the one that answers with the `MX5S` magic.
Probing the shell port leaves a stray `K` in its input line, so the probe sends
`^U` to erase it; nothing is executed, as no newline is ever sent.

The button stays live as **Change port…** once connected, so you can move to a
different port without reloading — an OS may hand the device a different name on
a later boot, and it's easy to pick the wrong one of the two. A manual pick is
probed just like an automatic one, so choosing the shell port reports that
plainly instead of hanging. Cancelling the picker leaves the current connection
untouched.

Permissions are stored per origin. A `file://` page may not persist them
reliably, so if auto-connect doesn't stick, serve the directory over localhost —
that gives a stable origin and the grant survives.
Not available in Firefox or Safari, which don't implement Web Serial. It needs a
secure context: opening the file directly works in Chrome, but if
`navigator.serial` is missing, serve the directory over localhost instead.

**Python**

```sh
pip install pyserial pygame
python mx5_viewer.py COM7            # or /dev/ttyACM1 on Linux/macOS
```

Left-click to tap; click-drag to swipe.

| Input | Action |
|---|---|
| Mouse wheel | Encoder knob (scroll down = clockwise) |
| Wheel click (middle button) | Press the knob |
| Up / Down arrow | Encoder knob, one click counter-clockwise / clockwise |
| Enter | Press the knob (held while held) |
| Left / Right arrow | Real arrow keys — move the text cursor when naming |
| Letters, digits, punctuation, space, backspace, delete, home/end | Typed into the device |

Wheel deltas are normalised to notches and accumulated before a step is sent:
the unit depends on `deltaMode` (about 100 per notch in pixel mode, 3 in line
mode, 1 in page mode) and a precision trackpad emits a stream of small ones, so
sending a step per event would spin the knob wildly on a trackpad and unevenly
across browsers.

The arrow split is deliberate. Up/Down drive the knob because that is what it
does nearly everywhere; Left/Right are sent as real keys because in the naming
dialogs they move the text cursor, and elsewhere the knob only picks
Cancel/Save, which Up/Down already covers. Space types a space (rig names
contain them), so Enter alone is the knob press — which suits the naming dialog
too, since its `onAccepted` handler confirms on Enter.

Typing works because the naming dialog is a real Qt Quick Controls `TextField`
with `focus: true`, not a grid of tappable buttons, so it consumes ordinary key
events. The server exposes a **second** uinput device for this rather than
adding a keymap to the touch device, since Qt classifies a device by what it
advertises. As with touch, every held key is released on exit — a Shift left
down would corrupt input from the real UI. *Encoder keys are the browser
viewer only; the Python viewer predates this.*

**Evil blacklists three keyboard nodes.** inMusic patched Qt's keyboard manager
so that a keyboard at `/dev/input/event2`, `event3` or `event4` is silently
refused: those paths are hardcoded in `qevdevkeyboardmanager.cpp`'s string pool
beside a `"Not adding keyboard at %ls"` message that stock Qt does not contain.
Only keyboards are filtered — our touchscreen on `event2` works fine.

This cost a while to find, because every layer looked healthy: the key reached
uinput, the device appeared in `/proc/bus/input/devices` with `kbd` attached and
the right capability bits, and Evil simply never opened it. Setting
`QT_QPA_EVDEV_KEYBOARD_PARAMETERS=/dev/input/event3` with
`QT_LOGGING_RULES='qt.qpa.input*=true'` is what made Evil say why:

```
qt.qpa.input: Not adding keyboard at /dev/input/event3
```

Since a node number can't be requested, the server burns the bad ones: it
creates a keyboard, asks via `UI_GET_SYSNAME` which node it got, and if that node
is blacklisted keeps it open as padding — so the number stays taken — and tries
again. `srv.log` reports the node finally used.

**Idle cost is negligible**: the server holds no flow-control credit until a
viewer asks for a frame, and it never reads the framebuffer without credit — it
just wakes briefly to check for a client. It is also `Nice=10` and pinned to
CPU3, away from the audio path.

The service is ordered `After=usb-console.service` so systemd stops it *before*
the USB gadget is torn down. That ordering matters: a process still holding
`/dev/ttyGS1` open while the gadget is removed can hang shutdown — which is
exactly what happens if you background the server from a shell instead, where
systemd knows nothing about it.

### By hand (for iterating)

**Stop the installed service first.** It has `Restart=always`, so `kill` alone
brings it back five seconds later — and you end up with *two* servers. Busybox
has no `pkill`; use `killall`:

```sh
systemctl stop mx5-screen        # also prevents the restart
killall mx5_screen_server        # any manual copy
```

Two servers is not a harmless waste. Both hold `/dev/ttyGS1` open and **race on
reads**: each input message goes to whichever process reads it first, so a touch
DOWN can land in one and the UP in the other. The first never sees a release, so
the contact sticks down and a tap behaves like a long press; other taps get split
so neither server sees a complete one and the click does nothing. Their frame
writes interleave too, which looks like protocol corruption. The server now takes
an exclusive `flock` and refuses to start twice rather than let this happen
silently, because the symptoms point everywhere except the actual cause.

Then paste the deploy blob into the root shell (see below) and:

```sh
taskset -c 3 /tmp/mx5_screen_server -r 15 <> /dev/ttyGS1 >&0 2>/tmp/srv.log &
```

`<> /dev/ttyGS1 >&0` opens the port read-write as both stdin and stdout, so
frames go out and touch events come back on the same channel. Add `-v` to trace
input (each touch DOWN/UP with coordinates and inter-event timing, and encoder
turns/presses) into the log. Stop it with `killall mx5_screen_server` before
shutting down, for the reason above.

## Building and deploying

`./build.sh` cross-compiles with the repo's Bootlin glibc-2.31 toolchain (the
device has glibc 2.32) and writes `mx5_screen_server.deploy.sh` — a paste-able
`gzip`+`base64` blob, ~7 KB. Paste that into the device's root shell and busybox
reconstructs the binary in `/tmp`. No reflashing is needed to iterate, which is
what makes this practical to develop.

`/tmp` is tmpfs, so re-paste after a reboot.

## How it works

**Capture — `/dev/fb0`.** Qt uses the `eglfs_mali` backend, which presents
through fbdev, so the framebuffer really does hold the live composited screen.
(Had it been `eglfs_kms` this approach wouldn't work at all.) The framebuffer is
480x800 32bpp and **triple buffered** (virtual height 2400), so the server reads
`FBIOGET_VSCREENINFO.yoffset` every frame to find the currently visible buffer —
reading a fixed offset yields stale or torn frames.

**Pixels are BGR*X*.** The fourth byte is padding and reads as `0x00` on this
device. Treating it as alpha makes every pixel fully transparent — which renders
a completely black window while the data is in fact perfect.

**Rotation.** The panel is mounted rotated: the framebuffer is portrait, the UI
is landscape. The viewer rotates for display and applies the exact inverse to
mouse coordinates.

**Encoding.** The screen is split into 32x32 tiles; only tiles that changed since
the last frame are sent, so an idle screen costs nothing. Tiles are RLE-encoded
over 32-bit pixels, which suits this UI — a flat tile goes from 4096 bytes to 6.
Tiles that don't compress fall back to raw, so a tile is never larger than raw.

**Flow control.** The server only sends a frame while it holds credit, granted by
the viewer's ack after each frame. This is essential, not an optimization: delta
frames cannot be dropped to catch up (each depends on the last), so without
pacing the device outruns the host and latency grows without bound. While the
server holds no credit it leaves its shadow buffer untouched, so changes
accumulate and coalesce into the next frame it may send.

**Stall recovery.** Flow control and resync interact badly if a frame is
truncated: the ack is only sent once a *complete* frame parses, and the server
sends nothing without credit, so a few lost bytes leave the parser waiting for a
tail that never comes and both ends wait on each other forever. The screen
freezes while input keeps working, since input travels the other direction. The
Python viewer escapes via pyserial's read timeout; Web Serial's `read()` has no
timeout, so the browser viewer runs a 1 s watchdog that drops the partial frame
and re-requests a keyframe after 2 s without one.

**Resync.** Every frame carries a self-describing header with a magic, so a
viewer can attach, detach and reattach at any time — a serial port has no
connection semantics, so there's no other way to know a client appeared. On any
malformed data the viewer drops its buffer, asks for a keyframe and resyncs
rather than dying.

**Input.** Touches are injected via `/dev/uinput` as an absolute multitouch
device, so they look like the real ili2116 touchscreen to Qt.

The server never exits holding a contact down — it releases on `SIGTERM`/`SIGINT`
and via `atexit`. This matters more than it sounds: if it dies mid-touch, the last
thing Qt saw was a press with no release, so whichever QML item took that press
keeps its **grab**. Qt grabs are scene-wide rather than per-device, so the
*physical* touchscreen stops responding too, and only a reboot clears it.

**Encoder.** The knob is *not* an input device — it's the control-surface MCU
sending MIDI over a serial link (`snd-serdev-midi`), which Evil consumes through
the ALSA sequencer. So uinput can't reach it; the server has to become a MIDI
source. Evil creates one ALSA seq client per MIDI device:

```
client  20: 'HG04 Control Surface' [kernel]  ──▶ client 129: 'Midi::In::HG04 Control Surface MIDI 1'
```

That client-129 port accepts writes, and Evil attributes events by *which of its
own ports* they arrive on — so events we send there are indistinguishable from
the real control surface, and the stock assignment file maps them for free:

| Message | Assignment target |
|---|---|
| CC 3 | `JogOutput` → `/Engine/PushEncoderCtrl/Encoder` (turn) |
| Note 4 | `PressAndHoldOutput` → `/Engine/PushEncoderCtrl/EncoderEnter` (push) |

The port is located by **name**, since seq client numbers are assigned
dynamically. `libasound` is `dlopen`ed, so the screen share still works if it's
unavailable — only the encoder is lost.

`JogOutput`'s relative-CC convention isn't documented in the firmware, so `-j`
selects it. The MX5 uses **signed bit** (`-j 1`, the default: −1 → 65), confirmed
on hardware. `-j 0` is two's complement (−1 → 127) and `-j 2` binary offset
(0 → 64), kept for other devices or firmware.

**Scheduling.** Pin to a core away from audio. On this device CPU0 services the
I2S DMA IRQ plus ~20 Evil threads and CPU2 carries ~14 (the DSP pool), while CPU3
has one — hence `taskset -c 3`. (The RK3288 is quad-core; a claim elsewhere in
this repo that the device has a single core is incorrect.)

## Performance notes, learned the hard way

The link sustains **~12 MB/s** (USB high-speed CDC-ACM), which is ~8 full
uncompressed frames per second before any compression — so no image codec is
needed on the device.

Two host-side mistakes each destroyed performance, and both were measured:

1. **Driver buffer too small.** Leaving pyserial's Windows RX buffer at its 4 KB
   default throttled 12 MB/s down to 0.6 MB/s. Hence `set_buffer_size`. The
   browser viewer has the same hazard in a different guise: Web Serial's
   `open({bufferSize})` defaults to **255 bytes**, so it passes 1 MB explicitly.
2. **Asking `read()` for more than needed.** `read(n)` blocks until n bytes
   arrive *or the timeout expires*, so requesting 64 KB "for batching" makes
   every read wait out the timeout instead of returning available data. This
   produced multi-second lag, and mid-frame timeouts then caused stream
   desyncs that looked like device-side corruption. `Reader._fill` now asks for
   exactly the deficit and then drains `in_waiting`, which batches without ever
   waiting. Web Serial avoids this trap by construction -- its `read()` returns
   whatever has arrived rather than waiting for a requested count.

## Limitations

* Animated regions (tuner needle, level meters) change every frame and will use
  real bandwidth; this is built for operating the UI, not watching animation.
* Touch injection relies on Qt picking up a uinput device created *after* Evil
  started. It works on this firmware, but a device created later isn't
  guaranteed to be enumerated by an already-running Qt app.
* Touch injection and the framebuffer are both read/written as root; the server
  runs as root out of necessity.
