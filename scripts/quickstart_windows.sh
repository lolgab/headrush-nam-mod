#!/usr/bin/env bash
# quickstart_windows.sh -- easiest path to a NAM-modded HeadRush updater for
# Windows. Run this from Linux, or from WSL2 on Windows itself (Docker
# Desktop's WSL2 backend makes docker available inside your WSL distro with
# no extra setup -- see https://docs.docker.com/desktop/wsl/).
#
# 1. Checks required tools are installed (docker, 7z, git) -- prints the
#    exact install commands and exits if anything is missing, never installs
#    anything itself.
# 2. Downloads the official HeadRush Pedalboard 2.7 Windows firmware
#    updater .exe.
# 3. Extracts its embedded Update.img, builds the NAM-modded version via
#    docker (scripts/build_docker.sh -- pins exact toolchain/e2fsprogs/
#    u-boot-tools versions so it works the same regardless of host distro),
#    and repacks a new .exe with scripts/repack_windows_updater.py.
# 4. Leaves the patched .exe and an untouched copy of the stock updater in
#    the current directory.
#
# Copy the patched .exe to your Windows machine and run it like the
# original updater to flash your device.
set -euo pipefail

FW_URL="https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/Pedalboard%20v2.7/Windows%20Updater/HeadRush%20Pedalboard%202.7%20Firmware%20Updater%20-%20Win.exe.zip"

DIR="$(cd "$(dirname "$0")/.." && pwd)"

die() { echo "ERROR: $*" >&2; exit 1; }

# ---- 1. tool check ---------------------------------------------------------
missing=()

check() {
    # check "<install hint>" cmd
    local hint="$1" cmd="$2"
    command -v "$cmd" >/dev/null 2>&1 || missing+=("$hint")
}

check "docker -- install Docker Desktop (Windows/WSL2: https://docs.docker.com/desktop/wsl/) or docker-ce (Linux)" docker
check "7z -- Debian/Ubuntu/WSL: sudo apt install p7zip-full" 7z
check "unzip -- Debian/Ubuntu/WSL: sudo apt install unzip" unzip
check "git -- Debian/Ubuntu/WSL: sudo apt install git" git
check "curl -- Debian/Ubuntu/WSL: sudo apt install curl" curl

if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing required tools:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi

docker info >/dev/null 2>&1 || die "docker CLI found but the daemon isn't reachable -- start Docker Desktop (or the docker service) and retry."

if [ ! -e "$DIR/nam_core/CMakeLists.txt" ]; then
    echo "nam_core submodule not initialized -- fetching it..."
    git -C "$DIR" submodule update --init --recursive
fi

# ---- 2. download the stock Windows updater ---------------------------------
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "Downloading stock HeadRush Pedalboard 2.7 Windows updater..."
curl -fL -o "$WORK/updater.zip" "$FW_URL"

echo "Extracting..."
mkdir -p "$WORK/extracted"
unzip -q -o "$WORK/updater.zip" -d "$WORK/extracted"

STOCK_EXE="$(find "$WORK/extracted" -maxdepth 1 -iname '*.exe' -print -quit)"
[ -n "$STOCK_EXE" ] || die "could not find a .exe in the downloaded zip -- HeadRush may have changed the updater layout."

STOCK_DEST="$PWD/$(basename "$STOCK_EXE")"
cp -f "$STOCK_EXE" "$STOCK_DEST"

# ---- 3. pull Update.img out of the installer, patch it, repack ------------
echo "Extracting Update.img from the stock installer..."
mkdir -p "$WORK/stock_payload"
7z x -o"$WORK/stock_payload" "$STOCK_EXE" -y >/dev/null
STOCK_IMG="$WORK/stock_payload/Update.img"
[ -f "$STOCK_IMG" ] || die "could not find Update.img inside $STOCK_EXE -- HeadRush may have changed the updater layout."

echo "Building NAM-modded Update.img via docker (this cross-compiles NAM's DSP core, a few minutes)..."
"$DIR/scripts/build_docker.sh" "$STOCK_IMG" "$WORK/Update_nam.img"

DEST_NAME="$(basename "$STOCK_EXE" .exe) (NAM mod).exe"
DEST="$PWD/$DEST_NAME"

echo "Repacking the Windows installer..."
python3 "$DIR/scripts/repack_windows_updater.py" "$STOCK_EXE" "$WORK/Update_nam.img" "$DEST"

echo
echo "Updater patched with the NAM mod:"
echo "  $DEST"
echo
echo "Unmodified stock updater (keep this for recovery):"
echo "  $STOCK_DEST"
echo
echo "Copy the patched .exe to your Windows machine (if built from WSL/Linux)"
echo "and run it like the official updater, with your device in"
echo "firmware-update mode. See README.md for recovery steps if a flash goes wrong."
echo
echo "Note: this repacked .exe is unsigned (the stock one is Authenticode-signed;"
echo "modifying it invalidates that signature, so it's stripped -- see"
echo "repack_windows_updater.py). Windows SmartScreen may warn about an"
echo "unrecognized publisher; this is expected for any unofficial build."
