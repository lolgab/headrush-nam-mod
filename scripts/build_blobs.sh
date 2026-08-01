#!/usr/bin/env bash
# build_blobs.sh -- (re)builds blobs/{libnam_hook.so,libnam_preload.so,
# trampoline_gonk.bin} from patch/*.cpp, via the docker/Dockerfile ARM
# cross-toolchain. The actual compile logic lives in build_blobs_native.sh
# (also used directly, without Docker, by .github/workflows/gui-build.yml's
# Linux CI runner) -- this script is just the Docker wrapper for local dev
# on any host OS.
#
# These are pure build ARTIFACTS -- deliberately NOT committed to git (see
# .gitignore): they're a fixed function of patch/*.cpp + nam_core, so
# there's nothing for a git diff to usefully show, and it keeps a stale
# blob from ever silently surviving a patch/*.cpp change. Run this
# yourself before building locally, or let CMake do it automatically (see
# CMakeLists.txt) -- either way it always rebuilds from source, never
# trusts a possibly-stale checked-in binary.
#
# Usage: ./scripts/build_blobs.sh
set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="headrush-nam-builder"

die() { echo "ERROR: $*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker not found in PATH -- install Docker Desktop (macOS/Windows) or docker-ce (Linux)."

echo "Building $IMAGE docker image (cached layers reused if docker/Dockerfile is unchanged)..."
docker build -t "$IMAGE" -f "$DIR/docker/Dockerfile" "$DIR"

"$DIR/scripts/fetch_nam_core.sh"
mkdir -p "$DIR/blobs"

docker run --rm \
    -v "$DIR":/repo \
    -w /repo \
    "$IMAGE" \
    scripts/build_blobs_native.sh

echo "OK  wrote $DIR/blobs/{libnam_hook.so,libnam_preload.so,trampoline_gonk.bin}"
