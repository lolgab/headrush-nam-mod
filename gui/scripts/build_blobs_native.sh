#!/usr/bin/env bash
# build_blobs_native.sh -- the actual ARM cross-compile logic for
# gui/blobs/{libnam_hook.so,libnam_preload.so,trampoline_gonk.bin}.
# Assumes the prefixed Bootlin ARM toolchain (arm-buildroot-linux-
# gnueabihf-{g++,as,objcopy,strip}) is already on PATH and nam_core/ is
# already populated (scripts/fetch_nam_core.sh) -- neither is this
# script's job to set up.
#
# Two callers, same logic, no duplicated flags:
#   - build_blobs.sh runs this INSIDE the docker/Dockerfile container
#     (for local dev on any host OS).
#   - .github/workflows/gui-build.yml runs this directly on an
#     ubuntu-latest runner after installing the same Bootlin toolchain
#     tarball natively (no Docker needed in CI -- the runner OS is
#     already the right one).
#
# Usage: ./gui/scripts/build_blobs_native.sh (run from the repo root)
set -euxo pipefail

NAM_CORE=nam_core
WORK=gui/blobs
CXX=arm-buildroot-linux-gnueabihf-g++
AS=arm-buildroot-linux-gnueabihf-as
OBJCOPY=arm-buildroot-linux-gnueabihf-objcopy
STRIP=arm-buildroot-linux-gnueabihf-strip

mkdir -p "$WORK"

"$CXX" -std=c++2a -O3 -ffast-math -fPIC -shared -march=armv7-a -mfpu=neon-vfpv4 \
    -mfloat-abi=hard -DNAM_ENABLE_A2_FAST -DNAM_SAMPLE_FLOAT \
    -static-libstdc++ -static-libgcc \
    -I"$NAM_CORE" -I"$NAM_CORE"/NAM -I"$NAM_CORE"/Dependencies/eigen -I"$NAM_CORE"/Dependencies/nlohmann \
    patch/nam_hook.cpp "$NAM_CORE"/NAM/*.cpp "$NAM_CORE"/NAM/wavenet/*.cpp \
    -lpthread -o "$WORK"/libnam_hook.unstripped.so

"$CXX" -std=c++17 -O2 -fPIC -shared -march=armv7-a -mfpu=neon-vfpv4 \
    -mfloat-abi=hard patch/nam_preload.cpp -ldl -o "$WORK"/libnam_preload.unstripped.so

"$AS" -mfpu=neon-vfpv4 -mfloat-abi=hard patch/trampoline_gonk.S -o "$WORK"/trampoline_gonk.o
"$OBJCOPY" -O binary "$WORK"/trampoline_gonk.o "$WORK"/trampoline_gonk.bin

"$STRIP" --strip-unneeded -o "$WORK"/libnam_hook.so "$WORK"/libnam_hook.unstripped.so
"$STRIP" --strip-unneeded -o "$WORK"/libnam_preload.so "$WORK"/libnam_preload.unstripped.so
rm -f "$WORK"/libnam_hook.unstripped.so "$WORK"/libnam_preload.unstripped.so "$WORK"/trampoline_gonk.o

echo "OK  wrote $WORK/{libnam_hook.so,libnam_preload.so,trampoline_gonk.bin}"
