#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the portable TuxOffice runner inside an Ubuntu 24.04 container.
#
# Run by packaging/build-release.sh; not meant to be run on a desktop.
# /src is the repository (read-only), /cache keeps downloads and compiler
# work between releases, /out receives the runner.
#
# Ubuntu 24.04 has glibc 2.39, so the runner works on every distribution the
# Soda base supports (which itself needs glibc 2.38).
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential clang lld llvm bison flex pkg-config git python3 ca-certificates curl \
  xz-utils zstd file meson ninja-build glslang-tools \
  libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libxkbregistry-dev libegl-dev libvulkan-dev \
  libfreetype-dev libfontconfig-dev libgnutls28-dev libsecret-1-dev libdbus-1-dev libsystemd-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libsdl2-dev \
  libx11-dev libxext-dev libxrandr-dev libxrender-dev libxi-dev libxcursor-dev libxfixes-dev \
  libxcomposite-dev libxinerama-dev libgl-dev >/dev/null

git config --global --add safe.directory '*'
rm -rf /work && mkdir -p /work
# Build exactly what is committed.
git clone -q --no-hardlinks /src /work/src

# The pinned Soda release the overlay is laid over.
eval "$(grep -E '^(RUNNER_NAME|RUNNER_URL|RUNNER_SHA256)=' /work/src/tuxoffice)"
mkdir -p /cache/downloads /work/home/runners
if [[ ! -s /cache/downloads/$RUNNER_NAME.tar.xz ]]; then
  curl -fsSL --retry 3 -o "/cache/downloads/$RUNNER_NAME.tar.xz.part" "$RUNNER_URL"
  mv "/cache/downloads/$RUNNER_NAME.tar.xz.part" "/cache/downloads/$RUNNER_NAME.tar.xz"
fi
echo "$RUNNER_SHA256  /cache/downloads/$RUNNER_NAME.tar.xz" | sha256sum -c --quiet
tar -C /work/home/runners -xf "/cache/downloads/$RUNNER_NAME.tar.xz"

TUXOFFICE_HOME=/work/home TUXOFFICE_BUILD_DIR=/cache/build TUXOFFICE_DXVK_BUILD_DIR=/cache/dxvk \
  /work/src/runner/build.sh

runner=/work/home/runners/tuxoffice-${RUNNER_NAME#soda-}
[[ -f $runner/share/tuxoffice/dxvk/resource-pool-lock-v1 ]] || { echo "DXVK bundle missing" >&2; exit 1; }
[[ -x $runner/bin/tuxoffice-file-dialog ]] || { echo "file chooser helper missing" >&2; exit 1; }
# Development headers are not needed to run Office. Without Soda's legacy
# 32-bit Unix loader, Wine runs 32-bit Windows code (such as Office's
# installer) through WoW64 in 64-bit processes, so distributions need no
# 32-bit libraries at all.
rm -rf "$runner/include" "$runner/lib/wine/x86_64-unix/.debug" "$runner/lib/wine/i386-unix"
rm -rf /out/runner && cp -a "$runner" /out/runner
chown -R "${OUT_UID:-0}:${OUT_GID:-0}" /out/runner
echo ":: portable runner ready"
