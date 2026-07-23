#!/bin/bash
# quickstart_mac.sh -- easiest path to a NAM-modded HeadRush updater on macOS.
#
# 1. Checks all required tools are installed (prints `brew install ...`
#    commands and exits if anything is missing -- never installs anything
#    itself).
# 2. Downloads the official HeadRush Pedalboard 2.7 Mac firmware updater.
# 3. Extracts it, runs it through build.sh, and writes the patched
#    Update.img back into a copy of the updater .app. Also leaves an
#    untouched copy of the stock updater in the current directory, for
#    recovery.
# 4. Prints the paths to both, in the current directory.
#
# Run the patched .app like the original updater to flash your device.
set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"

die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(uname)" = "Darwin" ] || die "this script is macOS-only (uses ditto/codesign)."

# ---- model selection (which device's updater to fetch + patch) -------------
MODEL="pedalboard"
while [ $# -gt 0 ]; do
    case "$1" in
        --model)   MODEL="${2:-}"; shift 2 ;;
        --model=*) MODEL="${1#*=}"; shift ;;
        -h|--help) echo "usage: $(basename "$0") [--model pedalboard|mx5]"; exit 0 ;;
        *)         die "unknown argument: $1 (usage: $(basename "$0") [--model pedalboard|mx5])" ;;
    esac
done

case "$MODEL" in
    pedalboard)
        FW_URL="https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/Pedalboard%20v2.7/MacOS%20Updater/HeadRush%20Pedalboard%202.7%20Firmware%20Updater%20-%20Mac.zip" ;;
    mx5)
        FW_URL="https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/MX5%20v2.7/MacOS%20Updater/HeadRush%20MX5%202.7%20Firmware%20Updater%20-%20Mac.zip" ;;
    *)
        die "unknown --model '$MODEL' (known: pedalboard, mx5)" ;;
esac

# ---- 1. tool check ---------------------------------------------------------
missing_brew_cmds=()

have_any() {
    # have_any path [path ...] -- true if any candidate is runnable/exists
    for c in "$@"; do
        command -v "$c" >/dev/null 2>&1 && return 0
        [ -x "$c" ] && return 0
    done
    return 1
}

check() {
    # check "<brew command to fix it>" path [path ...]
    local fix="$1"; shift
    have_any "$@" || missing_brew_cmds+=("$fix")
}

check "brew install e2fsprogs" \
    /opt/homebrew/opt/e2fsprogs/sbin/debugfs debugfs
check "brew install u-boot-tools" \
    mkimage
check "brew tap messense/macos-cross-toolchains && brew install armv7-unknown-linux-gnueabihf" \
    /opt/homebrew/opt/armv7-unknown-linux-gnueabihf/bin/armv7-unknown-linux-gnueabihf-g++ armv7-unknown-linux-gnueabihf-g++
check "brew install xz" \
    xz
check "brew install python3" \
    python3

if [ ${#missing_brew_cmds[@]} -gt 0 ]; then
    echo "Missing required tools. Run:" >&2
    printf '  %s\n' "${missing_brew_cmds[@]}" >&2
    exit 1
fi

"$DIR/scripts/fetch_nam_core.sh"

# ---- 2. download + extract the stock updater -------------------------------
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "Downloading stock HeadRush Mac updater ($MODEL)..."
curl -fL -o "$WORK/updater.zip" "$FW_URL"

echo "Extracting..."
mkdir -p "$WORK/extracted"
ditto -x -k "$WORK/updater.zip" "$WORK/extracted"

APP="$(find "$WORK/extracted" -maxdepth 3 -iname '*.app' -print -quit)"
[ -n "$APP" ] || die "could not find a .app in the downloaded zip -- HeadRush may have changed the updater layout."

UPDATE_IMG="$(find "$APP" -iname 'Update.img' -print -quit)"
[ -n "$UPDATE_IMG" ] || die "could not find Update.img inside $APP -- HeadRush may have changed the updater layout."

STOCK_DEST="$PWD/$(basename "$APP")"
rm -rf "$STOCK_DEST"
cp -R "$APP" "$STOCK_DEST"

# ---- 3. patch it ------------------------------------------------------------
echo "Building NAM-modded Update.img (this cross-compiles NAM's DSP core, a few minutes)..."
"$DIR/build.sh" "$UPDATE_IMG" "$WORK/Update_nam.img"

DEST_NAME="$(basename "$APP" .app) (NAM mod).app"
DEST="$PWD/$DEST_NAME"
rm -rf "$DEST"
cp -R "$APP" "$DEST"
cp "$WORK/Update_nam.img" "$DEST/${UPDATE_IMG#"$APP"/}"

if command -v codesign >/dev/null 2>&1; then
    codesign --force --deep --sign - "$DEST" 2>/dev/null \
        || echo "warning: ad-hoc re-sign failed, continuing anyway (usually harmless for a non-quarantined local copy)." >&2
fi

echo
echo "Updater patched with the NAM mod:"
echo "  $DEST"
echo
echo "Unmodified stock updater (keep this for recovery):"
echo "  $STOCK_DEST"
echo
echo "Run the patched one like the official updater, with your device in"
echo "firmware-update mode. See README.md for recovery steps if a flash goes wrong."
