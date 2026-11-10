#!/bin/sh
# build.sh -- cross-compile mx5_screen_server for the MX5 and emit a paste-able
# deploy blob.
#
# The device has a root shell over USB (tools/mx5_usb_console), so we don't need
# to reflash to iterate: gzip+base64 the binary, paste it into that shell, and
# busybox's base64/gunzip reconstruct it. A flash is only needed if you later
# want this to persist.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
OUT=${1:-$DIR/mx5_screen_server}

CC=$(ls "$ROOT"/.toolchain/*/bin/arm-buildroot-linux-gnueabihf-gcc 2>/dev/null | head -1)
[ -n "$CC" ] || { echo "ERROR: ARM toolchain not found in $ROOT/.toolchain -- see docker/Dockerfile" >&2; exit 1; }

"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -o "$OUT" "$DIR/mx5_screen_server.c" -ldl
"$(dirname "$CC")"/arm-buildroot-linux-gnueabihf-strip "$OUT"

echo "built: $OUT ($(wc -c < "$OUT") bytes)"
echo "max glibc required: $(readelf -V "$OUT" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1) (device has 2.32)"

# ---- deploy blob -----------------------------------------------------------
BLOB=$OUT.deploy.sh
{
    echo "# paste this whole block into the MX5 root shell (/dev/ttyGS0)"
    echo "cat > /tmp/s.gz.b64 <<'XEOF'"
    gzip -9c "$OUT" | base64 -w 100
    echo "XEOF"
    echo "base64 -d /tmp/s.gz.b64 | gunzip > /tmp/mx5_screen_server && chmod +x /tmp/mx5_screen_server && echo DEPLOYED \$(wc -c < /tmp/mx5_screen_server) bytes"
} > "$BLOB"
echo "deploy blob: $BLOB ($(wc -l < "$BLOB") lines, $(wc -c < "$BLOB") bytes)"
