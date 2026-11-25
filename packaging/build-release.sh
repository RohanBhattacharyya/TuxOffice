#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build TuxOffice's release files into dist/:
#   tuxoffice-VERSION-x86_64.tar.xz       portable tree (used by install.sh --user)
#   tuxoffice_VERSION-1_amd64.deb         Debian, Ubuntu and derivatives
#   tuxoffice-VERSION-1.x86_64.rpm        Fedora, openSUSE and derivatives
#   tuxoffice-VERSION-1-x86_64.pkg.tar.zst  Arch Linux and derivatives
#   SHA256SUMS
#
# Usage: packaging/build-release.sh [DIST]
# Needs docker (or podman as docker). The runner is built from the committed
# tree in an Ubuntu 24.04 container; set TUXOFFICE_REUSE_RUNNER=1 to package
# an existing DIST/runner again without rebuilding it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$(realpath -m "${1:-$ROOT/dist}")"
CACHE="${TUXOFFICE_RELEASE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/tuxoffice-release}"
VERSION="$(cat "$ROOT/VERSION")"
HOMEPAGE="${TUXOFFICE_HOMEPAGE:-https://github.com/RohanBhattacharyya/TuxOffice}"
NFPM_IMAGE=goreleaser/nfpm:v2.43.0
BUILD_IMAGE=ubuntu:24.04

msg() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null || die "docker is required"
cd "$ROOT"
[[ -z $(git status --porcelain --untracked-files=no) || ${TUXOFFICE_ALLOW_DIRTY:-0} == 1 ]] ||
  die "commit your changes first: releases are built from the committed tree"
mkdir -p "$DIST" "$CACHE"

if [[ ${TUXOFFICE_REUSE_RUNNER:-0} != 1 || ! -x $DIST/runner/bin/wine ]]; then
  msg "building the runner in $BUILD_IMAGE (this takes a while)"
  docker run --rm -v "$ROOT:/src:ro" -v "$CACHE:/cache" -v "$DIST:/out" \
    -e OUT_UID="$(id -u)" -e OUT_GID="$(id -g)" "$BUILD_IMAGE" bash /src/packaging/container-build.sh
fi

msg "checking the runner"
unix="$DIST/runner/lib/wine/x86_64-unix"
check() { # file text: the rebuilt library must reference this library
  grep -aq -- "$2" "$unix/$1" || die "$1 was built without $2; a build dependency is missing"
}
check winewayland.so libwayland-client.so.0
check winewayland.so libxkbcommon.so.0
check winewayland.so libxkbregistry.so.0
check win32u.so libfreetype.so.6
check win32u.so libfontconfig.so.1
check crypt32.so libgnutls.so.30
check winebus.so libSDL2-2.0.so.0
check windows.security.authentication.onlineid.so libsecret-1.so.0
check winegstreamer.so libgstreamer-1.0.so.0
[[ -x $DIST/runner/bin/tuxoffice-file-dialog ]] || die "the file chooser helper is missing"
[[ -x $DIST/runner/bin/tuxoffice-splash ]] || die "the launch window helper is missing"
[[ -f $DIST/runner/share/tuxoffice/dxvk/resource-pool-lock-v1 ]] || die "the DXVK bundle is missing"

msg "staging tuxoffice $VERSION"
stage="$DIST/stage"
rm -rf "$stage"
mkdir -p "$stage/usr/bin" "$stage/usr/share/tuxoffice" "$stage/usr/lib/tuxoffice" \
         "$stage/usr/share/doc/tuxoffice" "$stage/usr/share/licenses/tuxoffice"
git archive HEAD tuxoffice VERSION config lib | tar -x -C "$stage/usr/share/tuxoffice"
# The C helpers are already built into the runner.
find "$stage/usr/share/tuxoffice/lib" -name '*.c' -delete
cp -a "$DIST/runner" "$stage/usr/lib/tuxoffice/runner"
ln -s ../share/tuxoffice/tuxoffice "$stage/usr/bin/tuxoffice"
git show HEAD:README.md > "$stage/usr/share/doc/tuxoffice/README.md"
git show HEAD:LICENSE > "$stage/usr/share/licenses/tuxoffice/LICENSE"

rm -f "$DIST"/tuxoffice[-_]*
msg "writing the portable tarball"
tar -C "$stage" --owner=0 --group=0 -cf - usr | xz -T0 -6 > "$DIST/tuxoffice-$VERSION-x86_64.tar.xz"

for packager in deb rpm archlinux; do
  msg "writing the $packager package"
  docker run --rm -v "$ROOT:/src:ro" -v "$DIST:/dist" -v "$stage:/stage:ro" -w /src \
    -e TUXOFFICE_VERSION="$VERSION" -e TUXOFFICE_HOMEPAGE="$HOMEPAGE" \
    --user "$(id -u):$(id -g)" "$NFPM_IMAGE" \
    package --config packaging/nfpm.yaml --packager "$packager" --target /dist >/dev/null
done

msg "writing the AUR package description"
tarball_sum=$(sha256sum "$DIST/tuxoffice-$VERSION-x86_64.tar.xz" | cut -d' ' -f1)
rm -rf "$DIST/aur" && mkdir -p "$DIST/aur"
sed -e "s|@VERSION@|$VERSION|g" -e "s|@HOMEPAGE@|$HOMEPAGE|g" -e "s|@SHA256@|$tarball_sum|g" \
  packaging/arch/PKGBUILD.in > "$DIST/aur/PKGBUILD"
# .SRCINFO is what the AUR indexes; write it without needing makepkg.
{
  printf 'pkgbase = tuxoffice-bin\n\tpkgdesc = Microsoft 365 desktop apps on Linux, running natively\n'
  printf '\tpkgver = %s\n\tpkgrel = 1\n\turl = %s\n\tarch = x86_64\n\tlicense = LGPL-2.1-or-later\n' "$VERSION" "$HOMEPAGE"
  sed -n "/^depends=/,/)/p" "$DIST/aur/PKGBUILD" | grep -o "'[^']*'" | tr -d "'" | sed 's/^/\tdepends = /'
  sed -n "/^optdepends=/,/)/p" "$DIST/aur/PKGBUILD" | grep -o "'[^']*'" | tr -d "'" | sed 's/^/\toptdepends = /'
  printf '\tprovides = tuxoffice\n\tconflicts = tuxoffice\n\toptions = !strip\n\toptions = !debug\n'
  printf '\tsource = %s/releases/download/v%s/tuxoffice-%s-x86_64.tar.xz\n\tsha256sums = %s\n\n' \
    "$HOMEPAGE" "$VERSION" "$VERSION" "$tarball_sum"
  printf 'pkgname = tuxoffice-bin\n'
} > "$DIST/aur/.SRCINFO"

(cd "$DIST" && sha256sum tuxoffice[-_]* > SHA256SUMS)
msg "release files in $DIST:"
(cd "$DIST" && ls -sh tuxoffice[-_]* SHA256SUMS)
