#!/bin/sh
# Release the USB serial console gadget so another mode can claim the UDC.
# [added by mod]
G=/sys/kernel/config/usb_gadget/g3
[ -e "$G" ] || exit 0
echo "" > "$G/UDC" 2>/dev/null
rm -f  "$G/configs/c.1/acm.usb0"          2>/dev/null
rm -f  "$G/configs/c.1/acm.usb1"          2>/dev/null
rmdir  "$G/configs/c.1/strings/0x409"     2>/dev/null
rmdir  "$G/configs/c.1"                   2>/dev/null
rmdir  "$G/functions/acm.usb0"            2>/dev/null
rmdir  "$G/functions/acm.usb1"            2>/dev/null
rmdir  "$G/strings/0x409"                 2>/dev/null
rmdir  "$G"                               2>/dev/null
exit 0
