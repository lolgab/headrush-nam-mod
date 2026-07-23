#!/usr/bin/env bash
# build_docker.sh -- run build_update_img.py inside docker/Dockerfile instead
# of relying on host-installed tools. Works identically on macOS, Linux, and
# Windows/WSL2 (anywhere Docker Desktop or a Linux docker engine is
# available), which is the whole point: debugfs/mkimage/xz on-disk-format
# compat is version-sensitive, so this pins exact versions instead of
# depending on whatever the host distro happens to ship.
#
# Usage: ./scripts/build_docker.sh [--rebuild] <input Update.img> <output Update.img> [build_update_img.py args...]
# Any args after the output path are passed straight to build_update_img.py
# inside the container, e.g. `--model mx5`.
set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="headrush-nam-builder"

die() { echo "ERROR: $*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker not found in PATH -- install Docker Desktop (macOS/Windows) or docker-ce (Linux)."

NOCACHE=""
if [ "${1:-}" = "--rebuild" ]; then
    NOCACHE="--no-cache"
    shift
fi

[ $# -ge 2 ] || die "usage: $0 [--rebuild] <input Update.img> <output Update.img> [build_update_img.py args...]"

# Always run docker build (not just when the image is missing). Docker's layer
# cache makes this a near-instant no-op when docker/Dockerfile is unchanged, and
# -- crucially -- it rebuilds when the Dockerfile HAS changed. The old "skip if
# the image already exists" logic meant a pulled Dockerfile change (e.g. the
# glibc-2.31 toolchain swap) was silently ignored in favor of a stale cached
# image, so you could keep building boot-hanging firmware without knowing.
# `--rebuild` forces a full, cache-ignoring rebuild.
echo "Building $IMAGE docker image (cached layers reused if docker/Dockerfile is unchanged)..."
docker build $NOCACHE -t "$IMAGE" -f "$DIR/docker/Dockerfile" "$DIR"

"$DIR/scripts/fetch_nam_core.sh"

in_path="$1"
out_path="$2"
shift 2  # remaining args ("$@") are forwarded to build_update_img.py below
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
    python3 scripts/build_update_img.py "/in/$in_name" "/out/$out_name" "$@"
