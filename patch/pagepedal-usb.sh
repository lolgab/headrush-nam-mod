#!/bin/sh
# pagepedal-usb.sh -- USB gadget glue for the footswitch page-turner mod.
# Installed as /usr/Evil/Scripts/pagepedal-usb.sh by the NAM-mod builder.
#
# The RK3288 has ONE USB device controller (ff580000.usb) and the stock
# audio (gadget g1) / USB-drive (gadget g2) modes each bind it, so this
# gadget has to step aside for them -- the patched stock scripts call
# `release` before claiming the UDC and `idle` after they finish.
#
#   boot            idle + start the footswitch daemon (called from the
#                   launcher script in the background)
#   idle            bind a HID-keyboard-only gadget (g4) if the UDC is free.
#                   If the USB serial console mod's gadget (g3) exists, the
#                   keyboard is added to that instead of fighting it for the UDC
#   release         tear g4 down / remove the keyboard from g3, free the UDC
#   addhid <dir>    add the keyboard function to an existing, not-yet-bound
#                   gadget (used to give audio mode a keyboard too)
#   rmhid <dir>     undo addhid
#
# Deliberately not "set -e" and always exits 0: this must never be able to
# break boot or the stock USB modes.

# PAGEPEDAL_CONFIGFS: test hook only (point at a scratch dir); unset on the device
CONFIGFS=${PAGEPEDAL_CONFIGFS:-/sys/kernel/config}
CFG=$CONFIGFS/usb_gadget
G4=$CFG/g4
G3=$CFG/g3
UDC=ff580000.usb
# 45-byte boot-keyboard report descriptor (8-byte input report, no LEDs)
DESC='\005\001\011\006\241\001\005\007\031\340\051\347\025\000\045\001\165\001\225\010\201\002\225\001\165\010\201\003\225\006\165\010\025\000\045\145\005\007\031\000\051\145\201\000\300'
DESC_LEN=45

# Serialise: Evil's own mode scripts can fire at the same moment as boot.
exec 9> /tmp/pagepedal-usb.lock 2>/dev/null && flock 9 2>/dev/null

hid_add()
{
  G=$1
  [ -d "$G" ] || return 1
  F=$G/functions/hid.usb0
  [ -d "$F" ] || mkdir -p "$F" 2>/dev/null || return 1
  echo 1 > "$F/protocol"      2>/dev/null
  echo 1 > "$F/subclass"      2>/dev/null
  echo 8 > "$F/report_length" 2>/dev/null
  printf "$DESC" > "$F/report_desc" 2>/dev/null
  n=$(wc -c < "$F/report_desc" 2>/dev/null)
  if [ "$((n + 0))" != "$DESC_LEN" ]; then
    rmdir "$F" 2>/dev/null
    return 1
  fi
  [ -e "$G/configs/c.1/hid.usb0" ] || ln -s "$F" "$G/configs/c.1/hid.usb0" 2>/dev/null || {
    rmdir "$F" 2>/dev/null
    return 1
  }
  return 0
}

hid_rm()
{
  G=$1
  rm -f "$G/configs/c.1/hid.usb0" 2>/dev/null
  rmdir "$G/functions/hid.usb0" 2>/dev/null
  return 0
}

udc_busy_elsewhere()
{
  for g in "$CFG"/*; do
    [ -d "$g" ] || continue
    [ "$g" = "$G4" ] && continue
    [ -n "$(cat "$g/UDC" 2>/dev/null)" ] && return 0
  done
  return 1
}

idle()
{
  [ -d "$CONFIGFS" ] || return 0
  modprobe libcomposite 2>/dev/null

  if [ -d "$G3" ]; then
    # console mod present: share its gadget
    [ -e "$G3/configs/c.1/hid.usb0" ] && return 0
    cur=$(cat "$G3/UDC" 2>/dev/null)
    echo "" > "$G3/UDC" 2>/dev/null
    hid_add "$G3"
    [ -n "$cur" ] && echo "$cur" > "$G3/UDC" 2>/dev/null
    return 0
  fi

  [ -e "$G4" ] && return 0
  udc_busy_elsewhere && return 0

  mkdir -p "$G4" 2>/dev/null || return 0
  (
    cd "$G4" || exit 1
    echo 0x0763 > idVendor
    echo 0x5018 > idProduct
    mkdir -p strings/0x409
    SER=$(cat /tmp/evil-serial-number 2>/dev/null); [ -n "$SER" ] || SER=000000000000
    echo "$SER"                         > strings/0x409/serialnumber
    echo 'HeadRush'                     > strings/0x409/manufacturer
    echo 'HeadRush Footswitch Keyboard' > strings/0x409/product
    mkdir -p configs/c.1/strings/0x409
    echo 'Keyboard' > configs/c.1/strings/0x409/configuration
    echo 100        > configs/c.1/MaxPower
    hid_add "$G4" || exit 1
    echo "$UDC" > UDC 2>/dev/null || exit 1
  ) || release
  return 0
}

release()
{
  if [ -d "$G3" ] && [ -e "$G3/configs/c.1/hid.usb0" ]; then
    cur=$(cat "$G3/UDC" 2>/dev/null)
    echo "" > "$G3/UDC" 2>/dev/null
    hid_rm "$G3"
    # leave g3 unbound: the caller is about to claim the UDC itself
  fi
  [ -e "$G4" ] || return 0
  echo "" > "$G4/UDC" 2>/dev/null
  hid_rm "$G4"
  rmdir "$G4/configs/c.1/strings/0x409" 2>/dev/null
  rmdir "$G4/configs/c.1"               2>/dev/null
  rmdir "$G4/strings/0x409"             2>/dev/null
  rmdir "$G4"                           2>/dev/null
  return 0
}

case "$1" in
  boot)
    idle
    [ -x /usr/Evil/pagepedal ] && nohup /usr/Evil/pagepedal >> /tmp/pagepedal.log 2>&1 9>&- &
    ;;
  idle)    idle ;;
  release) release ;;
  addhid)  hid_add "$2" ;;
  rmhid)   hid_rm "$2" ;;
esac
exit 0
