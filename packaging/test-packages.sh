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
    # Each step runs as the test user from a script, so quoting stays simple
    # and any failing command fails the test.
    step() { # name, script on stdin
      echo "== $1"
      cat > /tmp/step.sh
      chmod 755 /tmp/step.sh
      sudo -iu tester bash -euo pipefail /tmp/step.sh
    }
    step "system install" <<"STEP"
bash /install.sh --from /dist --system --no-setup | tail -3
tuxoffice --version
tuxoffice doctor > /tmp/doctor.txt; head -2 /tmp/doctor.txt
STEP
    step "Wine environment" <<"STEP"
export WINEPREFIX=$HOME/p WINEDEBUG=-all
R=/usr/lib/tuxoffice/runner/bin
$R/wine --version
$R/wine wineboot -i >/dev/null 2>&1
# The registry reaches system.reg when the wineserver exits.
$R/wineserver -w
test -s "$HOME/p/system.reg"
$R/wine C:/windows/syswow64/cmd.exe /c echo 32-bit programs run 2>/dev/null | tr -d "\r" | grep "32-bit programs run"
$R/wine cmd /c echo 64-bit programs run 2>/dev/null | tr -d "\r" | grep "64-bit programs run"
$R/wineserver -k || true
STEP
    step "uninstall" <<"STEP"
bash /install.sh --uninstall | grep removed
! command -v tuxoffice
STEP
    step "user install" <<"STEP"
bash /install.sh --from /dist --user --no-setup | tail -2
$HOME/.local/bin/tuxoffice doctor > /tmp/doctor.txt; grep -q "runner .*ok" /tmp/doctor.txt
echo "per-user runner works"
STEP
  '
}

status=0
for image in "${images[@]}"; do
  printf '\n\033[1m### %s\033[0m\n' "$image"
  if run_test "$image"; then echo "PASS $image"; else echo "FAIL $image"; status=1; fi
done
exit $status
