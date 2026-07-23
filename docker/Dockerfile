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
    e2fsprogs \
    u-boot-tools \
    device-tree-compiler \
    xz-utils \
    bzip2 \
    curl \
    python3 \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ARM32 hard-float cross toolchain -- deliberately NOT Debian's
# crossbuild-essential-armhf. On bookworm that targets glibc 2.36, so the built
# shared libs require GLIBC_2.33/2.34/2.36 symbol versions the HeadRush device
# (glibc 2.32) can't resolve; Evil then fails to load the preload/hook libs and
# the device HANGS ON THE BOOT SPLASH. Bootlin's glibc-2.31 toolchain targets an
# old-enough glibc to run on the device. (build_update_img.py's
# check_glibc_compat() is the backstop that refuses too-new libs regardless.)
ARG TOOLCHAIN_URL=https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/armv7-eabihf--glibc--stable-2020.08-1.tar.bz2
RUN mkdir -p /opt/toolchain \
    && curl -fsSL "$TOOLCHAIN_URL" | tar -xj -C /opt/toolchain --strip-components=1
# /opt/toolchain/bin: the arm-buildroot-linux-gnueabihf-* cross tools.
# /usr/sbin,/sbin: debugfs/e2fsck live there on Debian (this image runs as root).
ENV PATH="/opt/toolchain/bin:/usr/sbin:/sbin:${PATH}"

WORKDIR /repo
