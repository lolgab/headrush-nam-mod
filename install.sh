#!/usr/bin/env bash
# install.sh -- true one-command install, no git clone needed:
#
#   curl -fsSL https://raw.githubusercontent.com/lolgab/headrush-nam-mod/main/install.sh | bash
#   curl -fsSL .../install.sh | bash -s -- --model mx5
#
# Downloads this repo as a plain tarball (no git required) into
# ./headrush-nam-mod, then runs the right quickstart script for your OS:
#   macOS        -> scripts/quickstart_mac.sh
#   WSL2         -> scripts/quickstart_windows.sh (builds a patched Windows .exe)
#   native Linux -> scripts/quickstart_linux.sh (builds a raw Update_nam.img)
# Any extra arguments (e.g. --model mx5) are forwarded as-is.
set -euo pipefail

REPO_URL="https://github.com/lolgab/headrush-nam-mod"
REF="main"
DEST="headrush-nam-mod"

die() { echo "ERROR: $*" >&2; exit 1; }

command -v curl >/dev/null 2>&1 || die "curl not found in PATH."
command -v tar  >/dev/null 2>&1 || die "tar not found in PATH."

if [ -e "$DEST" ]; then
    echo "Using existing ./$DEST/ (already downloaded)."
else
    echo "Downloading headrush-nam-mod ($REF)..."
    WORK="$(mktemp -d)"
    trap 'rm -rf "$WORK"' EXIT
    curl -fL -o "$WORK/repo.tar.gz" "$REPO_URL/archive/refs/heads/$REF.tar.gz"
    mkdir -p "$DEST"
    tar -xzf "$WORK/repo.tar.gz" -C "$DEST" --strip-components=1
fi

cd "$DEST"

case "$(uname -s)" in
    Darwin)
        exec ./scripts/quickstart_mac.sh "$@" ;;
    Linux)
        if grep -qi microsoft /proc/version 2>/dev/null; then
            echo "WSL detected -- building a patched Windows updater .exe."
            exec ./scripts/quickstart_windows.sh "$@"
        else
            exec ./scripts/quickstart_linux.sh "$@"
        fi
        ;;
    *)
        die "unsupported OS: $(uname -s) -- see README.md for manual steps." ;;
esac
