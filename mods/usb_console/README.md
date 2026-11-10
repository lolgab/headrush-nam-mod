# MX5 USB serial console

Adds a **root shell over USB** to HeadRush MX5 firmware 2.7. After flashing, plug
the MX5 into a computer and it enumerates as **two** USB serial ports:

| Port | Purpose |
|---|---|
| `ttyGS0` (lower COM number) | root shell |
| `ttyGS1` | free data channel, used by `tools/mx5_remote_screen` |

Opening the first gives a shell on the device.

Confirmed working on real MX5 hardware (appears as a COM port on Windows).

```sh
./build_usb_console.py <stock Update.img> <output Update.img>
```

Requires `debugfs` (e2fsprogs), `xz`, `mkimage` (u-boot-tools), Python 3. The
input file is never modified, and the script refuses to run on anything but MX5
2.7 (`compatible = "inmusic,hg04"`).

**Connecting**

| Host | How |
|---|---|
| Windows | new COM port under Device Manager → Ports; open in PuTTY |
| Linux / macOS | `screen /dev/ttyACM0` or `microcom /dev/ttyACM0` |

Baud rate is irrelevant (it's USB, not a real UART). The shell is already
running, so a fresh terminal may look blank — **press Enter** for a prompt.
It identifies as VID `0x0763` / PID `0x5017`, product "HeadRush MX5 Console".

## Why this is only configuration

The device already drives USB through the standard Linux **gadget configfs**
framework — that's how its audio-interface and USB-drive modes work (see
`/usr/Evil/Scripts/usb-otg-audio-start.sh`). And its kernel
(`5.10.96-inmusic-2022-02-02-rt60`) has **every gadget function built in**:

```
usb_f_acm   usb_f_ecm   usb_f_rndis  usb_f_ncm  usb_f_eem  u_ether
usb_f_mass_storage   usb_f_uac2_az01   usb_f_midi   libcomposite
```

So a serial console needs no kernel work at all — just an `acm` function plus a
couple of systemd units.

## What gets added

| Path | Purpose |
|---|---|
| `/usr/Evil/Scripts/setup-usb-console.sh` | create gadget `g3` with `acm.usb0` + `acm.usb1`, bind the UDC |
| `/usr/Evil/Scripts/remove-usb-console.sh` | tear it down, releasing the UDC |
| `/lib/systemd/system/usb-console.service` | run the setup at boot |
| `/lib/systemd/system/usb-console-shell.service` | `/bin/sh` on `/dev/ttyGS0`, auto-restart |
| symlinks in `/etc/systemd/system/multi-user.target.wants/` | enable both units |

and it **patches two stock scripts** — `usb-otg-audio-start.sh` and
`setup-mass-storage.sh` — to release the console gadget before they start.

### The single-UDC conflict

The RK3288 has exactly one USB device controller (`ff580000.usb`). The stock
audio mode (gadget `g1`) and USB-drive mode (gadget `g2`) each bind it, which is
why those modes are mutually exclusive in the first place. If the console gadget
held the UDC, they'd fail to start — hence the teardown calls. The console is
therefore available at idle, steps aside when you enter audio or USB-drive mode,
and comes back on the next boot.

(`libcomposite` is present, so a genuinely simultaneous multi-function gadget —
e.g. audio *and* console at once — is possible; this mod takes the simpler
mutually-exclusive route.)

## Security

`/etc/shadow` has an unknown `$6$` hash for root, so `login` would be unusable.
Rather than change the device's credentials, a shell is attached to the port
directly — which means **anyone with physical USB access gets root**. That's
reasonable for a debug tool on your own pedal, but it is not a hardened setup.
From the shell you can set a known root password and switch to
`serial-getty@ttyGS0.service` if you want authentication.

## What this unlocks

USB **networking** is the natural follow-on and needs no new kernel support
either — `usb_f_ecm` (Linux/macOS) and `usb_f_rndis` (Windows) are built in, and
`/sbin/ip`, `ifconfig` and busybox `udhcpc` are present. You can prototype it live
from this shell before committing anything to firmware:

```sh
cd /sys/kernel/config/usb_gadget/g3
echo "" > UDC                       # unbind
mkdir functions/ecm.usb0
ln -s functions/ecm.usb0 configs/c.1
echo ff580000.usb > UDC
ip addr add 10.0.0.1/24 dev usb0 && ip link set usb0 up
```

There is **no SSH server** in the rootfs (no dropbear/sshd; busybox here has
`inetd`/`login` but not `telnetd`), so SSH would mean adding a static ARM
`dropbear` binary.

**Screen mirroring** turned out to be practical after all — see
`tools/mx5_remote_screen`. Qt has no VNC platform plugin (eglfs only), but the
`eglfs_mali` backend presents through fbdev, so `/dev/fb0` holds the live
composited screen and can simply be read. Touch is injected back through
`/dev/uinput`. It streams over the `ttyGS1` channel this mod provides.

(An earlier version of this file claimed the device had a single core shared with
real-time audio, echoing a statement in the repo's own docs. That is wrong: the
RK3288 is quad-core, all four are online, and the firmware pins audio IRQs and
DSP threads to specific cores. Background work should still stay off the audio
core — `taskset -c 3` is the free one — but there is far more headroom than that
claim implies.)

## Risks and recovery

This is more invasive than a UI mod: it adds **boot-time systemd services** and
modifies the stock USB mode scripts. The setup script is deliberately defensive
(no `set -e`, exits 0 on any problem) so it can't block boot, and the teardown is
a no-op when the gadget isn't present. Firmware-update mode is a separate boot
path and is unaffected, so the recovery route stays intact:

> Hold the **first two footswitches** while powering on to force firmware-update
> mode, then reflash a stock `Update.img`.

Keep an unmodified `Update.img`. Use at your own risk.
