#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Install the release packages in clean containers of each supported
# distribution with install.sh, as a normal user with sudo, and check that
# the launcher and runner work and uninstall cleanly.
#
# Usage: packaging/test-packages.sh [DIST] [IMAGE...]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$(realpath "${1:-$ROOT/dist}")"
shift || true
images=("$@")
((${#images[@]})) || images=(ubuntu:24.04 debian:trixie fedora:42 opensuse/tumbleweed archlinux:latest)

run_test() { # image
  docker run --rm -v "$DIST:/dist:ro" -v "$ROOT/install.sh:/install.sh:ro" "$1" bash -euc '
    if command -v apt-get >/dev/null; then
      export DEBIAN_FRONTEND=noninteractive; apt-get update -qq; apt-get install -y -qq sudo curl xz-utils >/dev/null
    elif command -v dnf >/dev/null; then dnf install -y -q sudo curl xz >/dev/null
    elif command -v zypper >/dev/null; then zypper -q -n install sudo curl xz >/dev/null
    elif command -v pacman >/dev/null; then pacman -Sy --noconfirm --needed sudo curl xz >/dev/null
    fi
    useradd -m tester
    echo "tester ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/tester
    as() { su - tester -c "$*"; }
    echo "== system install"
    as "bash /install.sh --from /dist --system --no-setup" 2>&1 | tail -4
    as "tuxoffice --version"
    as "/usr/lib/tuxoffice/runner/bin/wine --version"
    as "tuxoffice doctor" | head -2
    echo "== Wine environment"
    as "WINEPREFIX=\$HOME/p WINEDEBUG=-all /usr/lib/tuxoffice/runner/bin/wine wineboot -i >/dev/null 2>&1; ls \$HOME/p/system.reg"
    echo "== uninstall"
    as "bash /install.sh --uninstall" 2>&1 | grep -E "removed|error" || true
    command -v tuxoffice >/dev/null && { echo "tuxoffice still installed"; exit 1; } || true
    echo "== user install"
    as "bash /install.sh --from /dist --user --no-setup" 2>&1 | tail -2
    as "\$HOME/.local/bin/tuxoffice doctor" | head -2
  '
}

status=0
for image in "${images[@]}"; do
  printf '\n\033[1m### %s\033[0m\n' "$image"
  if run_test "$image"; then echo "PASS $image"; else echo "FAIL $image"; status=1; fi
done
exit $status
