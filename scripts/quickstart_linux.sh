#!/usr/bin/env bash
# quickstart_linux.sh -- easiest path to a NAM-modded HeadRush Update.img on
# Linux (native, not WSL -- see quickstart_windows.sh for the WSL/Windows path).
#
# 1. Checks required tools are installed (docker, 7z) -- prints the
#    exact install commands and exits if anything is missing, never installs
#    anything itself.
# 2. Downloads the official HeadRush Windows firmware updater .exe for the
#    selected --model (default: pedalboard; also mx5) -- this is just
#    a 7z self-extracting archive, no Windows needed to unpack it.
# 3. Extracts its embedded Update.img and builds the NAM-modded version via
#    docker (scripts/build_docker.sh -- pins exact toolchain/e2fsprogs/
#    u-boot-tools versions so it works the same regardless of host distro).
# 4. Leaves the patched Update_nam.img and an untouched copy of the stock
#    Update.img in the current directory.
#
# Flash Update_nam.img with whatever tool you'd normally use to flash the
# stock Update.img on your device (see README.md).
set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"

die() { echo "ERROR: $*" >&2; exit 1; }

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
        FW_URL="https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/Pedalboard%20v2.7/Windows%20Updater/HeadRush%20Pedalboard%202.7%20Firmware%20Updater%20-%20Win.exe.zip" ;;
    mx5)
        FW_URL="https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/MX5%20v2.7/Windows%20Updater/HeadRush%20MX5%202.7%20Firmware%20Updater%20-%20Win.exe.zip" ;;
    *)
        die "unknown --model '$MODEL' (known: pedalboard, mx5)" ;;
esac

# ---- 1. tool check ---------------------------------------------------------
missing=()

check() {
    # check "<install hint>" cmd
    local hint="$1" cmd="$2"
    command -v "$cmd" >/dev/null 2>&1 || missing+=("$hint")
}

check "docker -- install docker-ce: https://docs.docker.com/engine/install/" docker
check "7z -- Debian/Ubuntu: sudo apt install p7zip-full" 7z
check "unzip -- Debian/Ubuntu: sudo apt install unzip" unzip
check "curl -- Debian/Ubuntu: sudo apt install curl" curl

if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing required tools:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi

docker info >/dev/null 2>&1 || die "docker CLI found but the daemon isn't reachable -- start the docker service and retry."

"$DIR/scripts/fetch_nam_core.sh"

# ---- 2. download the stock Windows updater (just used as an Update.img source) --
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "Downloading stock HeadRush updater ($MODEL)..."
curl -fL -o "$WORK/updater.zip" "$FW_URL"

echo "Extracting..."
mkdir -p "$WORK/extracted"
unzip -q -o "$WORK/updater.zip" -d "$WORK/extracted"

STOCK_EXE="$(find "$WORK/extracted" -maxdepth 1 -iname '*.exe' -print -quit)"
[ -n "$STOCK_EXE" ] || die "could not find a .exe in the downloaded zip -- HeadRush may have changed the updater layout."

# ---- 3. pull Update.img out of the installer, patch it ---------------------
echo "Extracting Update.img from the stock installer..."
mkdir -p "$WORK/stock_payload"
7z x -o"$WORK/stock_payload" "$STOCK_EXE" -y >/dev/null
STOCK_IMG="$WORK/stock_payload/Update.img"
[ -f "$STOCK_IMG" ] || die "could not find Update.img inside $STOCK_EXE -- HeadRush may have changed the updater layout."

STOCK_DEST="$PWD/Update.img"
cp -f "$STOCK_IMG" "$STOCK_DEST"

echo "Building NAM-modded Update.img via docker (this cross-compiles NAM's DSP core, a few minutes)..."
DEST="$PWD/Update_nam.img"
"$DIR/scripts/build_docker.sh" "$STOCK_IMG" "$DEST" --model "$MODEL"

echo
echo "NAM-modded firmware image:"
echo "  $DEST"
echo
echo "Unmodified stock image (keep this for recovery):"
echo "  $STOCK_DEST"
echo
echo "Flash Update_nam.img the same way you'd flash the stock Update.img on your"
echo "device (see README.md), with the device in firmware-update mode."
