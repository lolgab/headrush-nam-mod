#!/usr/bin/env python3
"""
build_usb_console.py -- add a USB serial debug console (root shell) to HeadRush
MX5 firmware 2.7.

After flashing, plugging the MX5 into a computer makes it enumerate as a USB
serial port (COM port on Windows, /dev/ttyACM0 on Linux/macOS). Opening that port
gives a root shell on the device -- enough to explore the filesystem, read logs,
inspect the running Evil process, and prototype further changes live instead of
through flash cycles.

HOW IT WORKS
------------
The device drives USB with the standard Linux **gadget configfs** framework (see
/usr/Evil/Scripts/usb-otg-audio-start.sh). Its kernel (5.10.96-inmusic-rt60) has
every gadget function built in already -- including usb_f_acm, which is all a
serial console needs. So this adds no kernel code; it only adds configuration:

  * gadget "g3" with an `acm.usb0` function, bound to the SoC's UDC
  * a systemd unit to bring it up at boot
  * a second unit running /bin/sh on /dev/ttyGS0

SINGLE UDC / MODE CONFLICT
--------------------------
The RK3288 has exactly one USB device controller (`ff580000.usb`), and the stock
audio (gadget g1) and mass-storage (gadget g2) modes each bind it. If our console
gadget held the UDC, those modes would fail to start. So this also prepends a
teardown call to both mode scripts: they release the console gadget before
claiming the UDC. Net effect -- the console is available at idle, steps aside when
you enter audio/USB-drive mode, and returns on the next boot.

ROOT SHELL, NOT A LOGIN PROMPT
------------------------------
/etc/shadow has an unknown $6$ hash for root, so `login` would be unusable. Rather
than alter the device's credentials, a shell is attached to the port directly.
That means ANYONE WITH PHYSICAL USB ACCESS GETS ROOT. That is fine for a debug
tool on your own device; it is not a hardening-friendly configuration. Once you
have a shell you can set a known password and switch to serial-getty if you'd
rather have authentication.

Usage:
    ./build_usb_console.py <input Update.img> <output Update.img>

Composes with the other MX5 mods, e.g.:
    ./build_tuner_tempo.py  stock.img a.img
    ./build_usb_console.py  a.img     final.img

Never modifies the input. Recovery: hold the first two footswitches while
powering on to force firmware-update mode, then reflash a stock Update.img.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from update_img import UpdateImg  # noqa: E402

PAYLOAD = Path(__file__).resolve().parent / "payload"
SCRIPTS = "/usr/Evil/Scripts"
UNITS = "/lib/systemd/system"
WANTS = "/etc/systemd/system/multi-user.target.wants"

FILES = [
    ("setup-usb-console.sh",       f"{SCRIPTS}/setup-usb-console.sh",  "0100755"),
    ("remove-usb-console.sh",      f"{SCRIPTS}/remove-usb-console.sh", "0100755"),
    ("usb-console.service",        f"{UNITS}/usb-console.service",       "0100644"),
    ("usb-console-shell.service",  f"{UNITS}/usb-console-shell.service", "0100644"),
]
ENABLE = ["usb-console.service", "usb-console-shell.service"]

TEARDOWN = (
    "\n# Release the USB serial console gadget -- this SoC has a single UDC, which\n"
    "# the gadget below needs to claim. [added by build_usb_console.py]\n"
    "[ -x /usr/Evil/Scripts/remove-usb-console.sh ] && /usr/Evil/Scripts/remove-usb-console.sh\n"
)
# (mode script, anchor to insert the teardown before/after)
PATCH_SCRIPTS = [
    (f"{SCRIPTS}/usb-otg-audio-start.sh", "modprobe configfs", "before"),
    (f"{SCRIPTS}/setup-mass-storage.sh",  ". /usr/Evil/Scripts/def_vars", "after"),
]


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <input Update.img> <output Update.img>")

    with UpdateImg(sys.argv[1]) as img:
        print(f"OK  {sys.argv[1]}: compatible={img.metadata['compatible']!r}")
        if img.metadata["compatible"] != "inmusic,hg04":
            sys.exit(f"REFUSING: MX5 (inmusic,hg04) only, got {img.metadata['compatible']!r}")

        # sanity: the gadget framework this relies on must actually be present
        if not img.exists(f"{SCRIPTS}/usb-otg-audio-start.sh"):
            sys.exit("REFUSING: no usb-otg-audio-start.sh -- unexpected firmware layout")

        for local, dest, mode in FILES:
            img.write(dest, (PAYLOAD / local).read_bytes(), mode=mode)
            print(f"OK  added {dest}")

        for unit in ENABLE:
            img.symlink(f"{WANTS}/{unit}", f"{UNITS}/{unit}")
            print(f"OK  enabled {unit}")

        for path, anchor, where in PATCH_SCRIPTS:
            text = img.read(path).decode()
            if "remove-usb-console" in text:
                print(f"--  {path} already patched, skipping")
                continue
            if anchor not in text:
                sys.exit(f"REFUSING: anchor {anchor!r} not found in {path} "
                         f"-- firmware layout changed, re-verify before patching")
            repl = (TEARDOWN + "\n" + anchor) if where == "before" else (anchor + "\n" + TEARDOWN)
            img.write(path, text.replace(anchor, repl, 1), mode="0100755")
            print(f"OK  patched {path} to release the console gadget first")

        img.save(sys.argv[2])

    print("\nFlash with the official updater, then plug the MX5 into a computer:\n"
          "  Windows: a new COM port appears (Device Manager -> Ports); open it in PuTTY\n"
          "  Linux/macOS: screen /dev/ttyACM0   (baud rate is irrelevant over USB)\n"
          "Press Enter to get a prompt.")


if __name__ == "__main__":
    main()
