#!/usr/bin/env bash
# build_e2fsprogs_windows.sh -- builds the 5 e2fsprogs libraries
# core/ext4_image.c actually needs (com_err, ext2fs, e2p, uuid, blkid) for
# a Windows (mingw-w64) target, installing headers+static libs+pkg-config
# files into $PREFIX.
#
# No vcpkg or MSYS2 binary package for libext2fs exists (checked both
# indexes) -- but the e2fsprogs SOURCE itself builds cleanly for mingw-w64:
# configure.ac already special-cases `mingw*` hosts (selects windows_io.c
# as the I/O backend automatically -- see ext2_io.h's default_io_manager,
# which core/ext4_image.c already relies on instead of hardcoding
# unix_io_manager). Confirmed by actually building it (not just reading
# configure.ac) via a mingw-w64 cross-toolchain in Docker before writing
# this script.
#
# Deliberately builds ONLY these 5 subdirectories, not the whole tree:
# lib/ss (the interactive command-subsystem library used by debugfs's own
# shell, NOT by libext2fs itself) uses fork()/wait()/sigprocmask() and
# does not build for Windows at all -- irrelevant here, since this
# project only calls ext2fs_* functions directly, never lib/ss.
#
# Usage: ./gui/scripts/build_e2fsprogs_windows.sh [prefix]
#   Env overrides: HOST (default x86_64-w64-mingw32), CC (default: cc's
#   own default -- bare "gcc" under a native MinGW/MSYS2 shell; set to a
#   prefixed cross-compiler, e.g. x86_64-w64-mingw32-gcc-posix, when
#   cross-compiling from Linux).
set -euo pipefail

E2FSPROGS_VERSION=1.47.1
HOST="${HOST:-x86_64-w64-mingw32}"
PREFIX="${1:-$(cd "$(dirname "$0")/../.." && pwd)/gui/build/e2fsprogs-windows}"

die() { echo "ERROR: $*" >&2; exit 1; }

command -v curl >/dev/null 2>&1 || die "curl not found in PATH."
command -v make >/dev/null 2>&1 || die "make not found in PATH."

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "Downloading e2fsprogs $E2FSPROGS_VERSION source..."
curl -fsSL -o "$WORK/e2fsprogs.tar.gz" \
    "https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v$E2FSPROGS_VERSION/e2fsprogs-$E2FSPROGS_VERSION.tar.gz"

# --exclude the one symlink in the whole tarball (RELEASE-NOTES ->
# doc/RelNotes/v1.47.0.txt, doc-only, irrelevant to the build): creating
# it fails under MSYS2's tar on NTFS without elevated/Developer-Mode
# symlink privileges, which a stock GitHub Actions windows-latest runner
# doesn't have -- confirmed for real (this exact failure is what a real
# windows-latest CI run surfaced the first time this script ran there).
tar --exclude="e2fsprogs-$E2FSPROGS_VERSION/RELEASE-NOTES" -xzf "$WORK/e2fsprogs.tar.gz" -C "$WORK"
cd "$WORK/e2fsprogs-$E2FSPROGS_VERSION"

CC_ARGS=()
if [ -n "${CC:-}" ]; then
    CC_ARGS=(CC="$CC")
fi

echo "Configuring for host=$HOST..."
./configure --host="$HOST" --prefix="$PREFIX" \
    --disable-nls --disable-imager --disable-resizer --disable-defrag \
    --disable-uuidd --disable-fuse2fs --disable-e2initrd-helper \
    "${CC_ARGS[@]}"

for lib in lib/et lib/ext2fs lib/e2p lib/uuid lib/blkid; do
    echo "Building $lib..."
    make -C "$lib"
    make -C "$lib" install
done

echo "OK  installed e2fsprogs Windows libs to $PREFIX"
