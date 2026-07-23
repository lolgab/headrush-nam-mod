# Build environment for scripts/build_update_img.py.
#
# The build needs an ARM32 hard-float cross toolchain, e2fsprogs (debugfs/
# e2fsck), u-boot-tools (mkimage), and xz -- all version-sensitive enough
# (esp. e2fsprogs/mkimage's on-disk format compat) that pinning them in an
# image beats "whatever the host distro happens to have". This image is the
# same on macOS (via Docker Desktop), Linux, and Windows/WSL2 -- see
# scripts/build_docker.sh.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    crossbuild-essential-armhf \
    e2fsprogs \
    u-boot-tools \
    device-tree-compiler \
    xz-utils \
    python3 \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# debugfs/e2fsck live in /sbin on Debian; not on PATH by default for non-root
# login shells, but this image always runs as root anyway.
ENV PATH="/usr/sbin:/sbin:${PATH}"

WORKDIR /repo
