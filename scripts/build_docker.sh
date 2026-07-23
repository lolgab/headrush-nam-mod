#!/usr/bin/env bash
# build_docker.sh -- run build_update_img.py inside docker/Dockerfile instead
# of relying on host-installed tools. Works identically on macOS, Linux, and
# Windows/WSL2 (anywhere Docker Desktop or a Linux docker engine is
# available), which is the whole point: debugfs/mkimage/xz on-disk-format
# compat is version-sensitive, so this pins exact versions instead of
# depending on whatever the host distro happens to ship.
#
# Usage: ./scripts/build_docker.sh [--rebuild] <input Update.img> <output Update.img>
set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="headrush-nam-builder"

die() { echo "ERROR: $*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker not found in PATH -- install Docker Desktop (macOS/Windows) or docker-ce (Linux)."

REBUILD=0
if [ "${1:-}" = "--rebuild" ]; then
    REBUILD=1
    shift
fi

[ $# -eq 2 ] || die "usage: $0 [--rebuild] <input Update.img> <output Update.img>"

if [ "$REBUILD" = 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Building $IMAGE docker image..."
    docker build -t "$IMAGE" -f "$DIR/docker/Dockerfile" "$DIR"
fi

if [ ! -e "$DIR/nam_core/CMakeLists.txt" ]; then
    echo "nam_core submodule not initialized -- fetching it..."
    git -C "$DIR" submodule update --init --recursive
fi

in_path="$1"
out_path="$2"
[ -f "$in_path" ] || die "$in_path not found"

in_dir="$(cd "$(dirname "$in_path")" && pwd)"
in_name="$(basename "$in_path")"
mkdir -p "$(dirname "$out_path")"
out_dir="$(cd "$(dirname "$out_path")" && pwd)"
out_name="$(basename "$out_path")"

docker run --rm \
    -v "$DIR":/repo \
    -v "$in_dir":/in:ro \
    -v "$out_dir":/out \
    -w /repo \
    "$IMAGE" \
    python3 scripts/build_update_img.py "/in/$in_name" "/out/$out_name"
