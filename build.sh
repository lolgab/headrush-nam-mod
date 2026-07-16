#!/bin/sh
# Thin wrapper: ./build.sh path/to/stock/Update.img path/to/output/Update.img
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$DIR/scripts/build_update_img.py" "$@"
