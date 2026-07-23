#!/usr/bin/env bash
# fetch_nam_core.sh -- vendors nam_core (NeuralAmpModelerCore + Eigen headers)
# as plain source downloaded straight from GitHub/GitLab as tarballs, no git
# submodules or even git itself needed.
#
# Only what the build actually uses (see build_update_img.py) is fetched:
# NAM/*.cpp + NAM/wavenet/*.cpp + headers, Dependencies/nlohmann (vendored
# in the NeuralAmpModelerCore repo itself, not its own submodule), and
# Dependencies/eigen (header-only). NeuralAmpModelerCore's own
# Dependencies/AudioDSPTools submodule is never fetched -- it's only used by
# nam_core's own example app/tests, not by this repo's build.
set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"

# Pinned commits (kept in sync with what used to be the nam_core git
# submodule's checkout). Bump deliberately, not automatically.
NAM_CORE_REF="3cde95c354d5ba6da01316cad90b05cfc4855053"
EIGEN_REF="bc3b39870ecb690a623a3f49149a358b95c5781d"

die() { echo "ERROR: $*" >&2; exit 1; }

if [ -f "$DIR/nam_core/NAM/dsp.h" ] && [ -f "$DIR/nam_core/Dependencies/eigen/Eigen/Core" ]; then
    exit 0
fi

command -v curl >/dev/null 2>&1 || die "curl not found in PATH."
command -v tar  >/dev/null 2>&1 || die "tar not found in PATH."

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "Fetching NeuralAmpModelerCore (pinned commit ${NAM_CORE_REF:0:12})..."
curl -fL -o "$WORK/nam_core.tar.gz" \
    "https://github.com/sdatkinson/NeuralAmpModelerCore/archive/${NAM_CORE_REF}.tar.gz"
mkdir -p "$DIR/nam_core"
tar -xzf "$WORK/nam_core.tar.gz" -C "$DIR/nam_core" --strip-components=1

echo "Fetching Eigen (pinned commit ${EIGEN_REF:0:12})..."
curl -fL -o "$WORK/eigen.tar.gz" \
    "https://gitlab.com/libeigen/eigen/-/archive/${EIGEN_REF}/eigen-${EIGEN_REF}.tar.gz"
mkdir -p "$DIR/nam_core/Dependencies/eigen"
tar -xzf "$WORK/eigen.tar.gz" -C "$DIR/nam_core/Dependencies/eigen" --strip-components=1

[ -f "$DIR/nam_core/NAM/dsp.h" ] \
    || die "NAM/dsp.h missing after extraction -- NeuralAmpModelerCore may have changed layout."
[ -f "$DIR/nam_core/Dependencies/eigen/Eigen/Core" ] \
    || die "Eigen/Core missing after extraction -- Eigen may have changed layout."

echo "nam_core vendored (NAM sources + Eigen headers)."
