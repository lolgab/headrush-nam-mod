#!/usr/bin/env python3
"""
build_install_screen.py -- install the remote screen server into MX5 firmware so
it starts automatically at boot.

Without this you have to log in over the serial console and launch the server by
hand after every boot (and re-paste it, since /tmp is tmpfs). With it, the screen
share is available as soon as the device is up -- no shell needed.

Requires the USB gadget from tools/mx5_usb_console (it provides /dev/ttyGS1).
Chain the builds:

    ../mx5_usb_console/build_usb_console.py stock.img a.img
    ./build_install_screen.py               a.img     final.img

Run ./build.sh first to produce the ARM binary.

IDLE COST: negligible. The server holds no flow-control credit until a viewer
asks for a frame, and it never touches the framebuffer without credit -- it just
wakes briefly to check for a client. It is also niced and pinned to CPU3, away
from the audio path.

SHUTDOWN: the service orders itself After=usb-console.service, so systemd stops
it *before* the USB gadget is torn down. A process still holding /dev/ttyGS1
open while the gadget is removed can hang poweroff -- which is exactly what
happens if you background the server from the shell instead of running it as a
unit.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "mx5_usb_console"))
from update_img import UpdateImg  # noqa: E402

BINARY_SRC = HERE / "mx5_screen_server"
SERVICE_SRC = HERE / "payload" / "mx5-screen.service"
BINARY_DST = "/usr/Evil/mx5_screen_server"
SERVICE_DST = "/lib/systemd/system/mx5-screen.service"
WANTS = "/etc/systemd/system/multi-user.target.wants/mx5-screen.service"


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <input Update.img> <output Update.img>")
    if not BINARY_SRC.is_file():
        sys.exit(f"ERROR: {BINARY_SRC} not found -- run ./build.sh first")

    binary = BINARY_SRC.read_bytes()
    if binary[:4] != b"\x7fELF" or binary[18:20] != b"\x28\x00":   # EM_ARM
        sys.exit(f"ERROR: {BINARY_SRC} is not an ARM ELF -- was it built with the "
                 f"cross toolchain? (see build.sh)")

    with UpdateImg(sys.argv[1]) as img:
        print(f"OK  {sys.argv[1]}: compatible={img.metadata['compatible']!r}")
        if img.metadata["compatible"] != "inmusic,hg04":
            sys.exit(f"REFUSING: MX5 (inmusic,hg04) only, got {img.metadata['compatible']!r}")
        if not img.exists("/usr/Evil/Scripts/setup-usb-console.sh"):
            sys.exit("REFUSING: the USB gadget mod is not present in this image -- run "
                     "tools/mx5_usb_console/build_usb_console.py first (it creates "
                     "/dev/ttyGS1, which this server streams over)")

        img.write(BINARY_DST, binary, mode="0100755")
        print(f"OK  installed {BINARY_DST} ({len(binary)} bytes)")
        img.write(SERVICE_DST, SERVICE_SRC.read_bytes(), mode="0100644")
        print(f"OK  installed {SERVICE_DST}")
        img.symlink(WANTS, SERVICE_DST)
        print("OK  enabled mx5-screen.service")

        img.save(sys.argv[2])

    print("\nAfter flashing, the screen server runs from boot -- just start the viewer:\n"
          "    python mx5_viewer.py COM7\n"
          "No serial login required.")


if __name__ == "__main__":
    main()
