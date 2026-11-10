#!/bin/sh
# USB serial debug console (CDC-ACM gadget).  [added by mod]
#
# Creates gadget "g3" with TWO acm functions and binds the UDC, so the device
# shows up on the host as two USB serial ports (/dev/ttyACM0-1 on Linux/macOS,
# two COM ports on Windows):
#   ttyGS0 -- root shell, attached by usb-console-shell.service
#   ttyGS1 -- free data channel (used by tools/mx5_remote_screen)
#
# Both are created here rather than added later at runtime: configfs gadget
# state does not survive a reboot, so a channel added by hand disappears on
# every power cycle.
#
# NOTE: the RK3288 has a single UDC, and audio (gadget g1) / mass-storage
# (gadget g2) bind the same one, so those scripts call remove-usb-console.sh
# first to release it.  Deliberately not "set -e": this must never break boot.

UDC=ff580000.usb
G=/sys/kernel/config/usb_gadget/g3

[ -d /sys/kernel/config ] || exit 0
[ -e "$G" ] && exit 0                      # already set up

modprobe libcomposite 2>/dev/null

mkdir -p "$G" 2>/dev/null || exit 0
cd "$G" || exit 0

echo 0x0763 > idVendor
echo 0x5017 > idProduct                    # unused PID (audio=0x401A, msc=0x5016)
mkdir -p strings/0x409
SER=$(cat /tmp/evil-serial-number 2>/dev/null); [ -n "$SER" ] || SER=000000000000
echo "$SER"                 > strings/0x409/serialnumber
echo 'HeadRush'             > strings/0x409/manufacturer
echo 'HeadRush MX5 Console' > strings/0x409/product

mkdir -p configs/c.1/strings/0x409
echo 'Console' > configs/c.1/strings/0x409/configuration
echo 100       > configs/c.1/MaxPower

mkdir -p functions/acm.usb0
ln -s functions/acm.usb0 configs/c.1 2>/dev/null
mkdir -p functions/acm.usb1
ln -s functions/acm.usb1 configs/c.1 2>/dev/null

echo "$UDC" > UDC 2>/dev/null
exit 0
