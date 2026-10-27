#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# TuxOffice installer.
#
#   curl -fsSL https://raw.githubusercontent.com/RohanBhattacharyya/TuxOffice/main/install.sh | bash
#
# Installs TuxOffice with your distribution's package manager (apt, dnf,
# zypper or pacman) or, without root, into ~/.local; then offers to set it up
# and install Microsoft 365. Run "bash install.sh --help" for the options.
#
# Everything runs from main() at the end, so a download cut short does nothing.

set -euo pipefail

REPO="${TUXOFFICE_REPO:-RohanBhattacharyya/TuxOffice}"
MIN_GLIBC=2.38
USER_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/tuxoffice/app"
USER_BIN="$HOME/.local/bin"

if [[ -t 1 ]]; then
  B=$'\033[1m' DIM=$'\033[2m' BLUE=$'\033[1;34m' GREEN=$'\033[1;32m' YELLOW=$'\033[1;33m' RED=$'\033[1;31m' R=$'\033[0m'
else
  B="" DIM="" BLUE="" GREEN="" YELLOW="" RED="" R=""
fi
msg()  { printf '%s::%s %s\n' "$BLUE" "$R" "$*"; }
ok()   { printf '%s✓%s %s\n' "$GREEN" "$R" "$*"; }
warn() { printf '%swarning:%s %s\n' "$YELLOW" "$R" "$*" >&2; }
die()  { printf '%serror:%s %s\n' "$RED" "$R" "$*" >&2; exit 1; }

usage() {
  cat <<EOF
TuxOffice installer

usage: install.sh [options]

  --system          install with the package manager (default when supported)
  --user            install into your home folder, without root
  --office          then set up TuxOffice and install Microsoft 365
  --setup-only      then set up TuxOffice, but install Microsoft 365 later
  --no-setup        only install TuxOffice
  --version X.Y.Z   install this release instead of the latest
  --from DIR        use release files from DIR instead of downloading them
  --uninstall       remove TuxOffice (your Office installation and documents stay)
  -y, --yes         do not ask; use the defaults (system install, then setup only)
  -h, --help        show this help

Environment: TUXOFFICE_REPO=owner/name selects the GitHub repository.
EOF
}

# Questions are read from the terminal, also when the script itself arrives
# on standard input (curl | bash).
TTY=""
[[ -r /dev/tty ]] && { : </dev/tty; } 2>/dev/null && TTY=/dev/tty
ask() { # prompt default -> answer
  local answer
  if [[ $ASSUME_YES == 1 || -z $TTY ]]; then echo "$2"; return; fi
  printf '%s' "$1" >&2
  read -r answer <"$TTY" || answer=""
  echo "${answer:-$2}"
}
choose() { # title default option... -> chosen number
  local title=$1 default=$2 i=1 answer
  shift 2
  if [[ $ASSUME_YES == 1 || -z $TTY ]]; then echo "$default"; return; fi
  printf '\n%s%s%s\n' "$B" "$title" "$R" >&2
  for option in "$@"; do printf '  %s%d)%s %s\n' "$B" "$i" "$R" "$option" >&2; i=$((i + 1)); done
  while :; do
    answer=$(ask "Choose [${default}]: " "$default")
    [[ $answer =~ ^[0-9]+$ ]] && ((answer >= 1 && answer <= $#)) && { echo "$answer"; return; }
    printf 'Please enter a number from 1 to %d.\n' "$#" >&2
  done
}

sudo_cmd() {
  if [[ $EUID -eq 0 ]]; then "$@"
  elif command -v sudo >/dev/null; then sudo "$@"
  elif command -v doas >/dev/null; then doas "$@"
  else die "sudo is needed to install packages; or rerun with --user"
  fi
}

detect_family() {
  local id="" like=""
  if [[ -r /etc/os-release ]]; then
    # In a subshell: os-release defines VERSION and other common names.
    id=$(. /etc/os-release && echo "${ID:-}")
    like=$(. /etc/os-release && echo "${ID_LIKE:-}")
    DISTRO_NAME=$(. /etc/os-release && echo "${PRETTY_NAME:-${ID:-}}")
  fi
  [[ -e /run/ostree-booted ]] && { FAMILY=immutable; return; }
  for word in $id $like; do
    case $word in
      debian|ubuntu) FAMILY=deb; return ;;
      fedora|rhel|centos) FAMILY=rpm-dnf; return ;;
      suse|opensuse|opensuse-tumbleweed|opensuse-leap|sles) FAMILY=rpm-zypper; return ;;
      arch|archlinux) FAMILY=arch; return ;;
    esac
  done
  command -v pacman >/dev/null && { FAMILY=arch; return; }
  FAMILY=unknown
}

package_tool() {
  case $FAMILY in
    deb) echo apt ;;
    rpm-dnf) echo dnf ;;
    rpm-zypper) echo zypper ;;
    arch) echo pacman ;;
  esac
}

asset_name() { # family -> release file
  case $1 in
    deb) echo "tuxoffice_${TUX_VERSION}-1_amd64.deb" ;;
    rpm-dnf|rpm-zypper) echo "tuxoffice-${TUX_VERSION}-1.x86_64.rpm" ;;
    arch) echo "tuxoffice-${TUX_VERSION}-1-x86_64.pkg.tar.zst" ;;
    user) echo "tuxoffice-${TUX_VERSION}-x86_64.tar.xz" ;;
  esac
}

check_system() {
  [[ $(uname -s) == Linux ]] || die "TuxOffice runs on Linux"
  [[ $(uname -m) == x86_64 ]] || die "TuxOffice needs a 64-bit x86 (x86_64) system; this is $(uname -m)"
  [[ $EUID -ne 0 ]] || die "run the installer as your normal user, not root: Office is installed per user, and sudo is asked for when needed"
  local glibc
  glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}') || glibc=""
  if [[ -n $glibc ]] && [[ $(printf '%s\n%s\n' "$MIN_GLIBC" "$glibc" | sort -V | head -1) != "$MIN_GLIBC" ]]; then
    die "glibc $glibc is too old; TuxOffice needs $MIN_GLIBC or newer (Ubuntu 24.04, Debian 13, Fedora 40, openSUSE Tumbleweed, Arch or newer)"
  fi
  [[ -z $glibc ]] && warn "could not determine the C library version; TuxOffice needs glibc $MIN_GLIBC or newer"
  command -v curl >/dev/null || die "curl is required"
  if ! ls /usr/share/vulkan/icd.d/*.json /etc/vulkan/icd.d/*.json >/dev/null 2>&1; then
    warn "no Vulkan driver found; install your GPU's Vulkan driver (Mesa or NVIDIA) for Office to render"
  fi
  [[ -n ${WAYLAND_DISPLAY:-} ]] || warn "not in a Wayland session; TuxOffice is developed on Wayland (X11 works but is less tested)"
}

resolve_version() {
  [[ -n $TUX_VERSION ]] && return
  if [[ -n $FROM ]]; then
    TUX_VERSION=$(cd "$FROM" && ls tuxoffice-*-x86_64.tar.xz 2>/dev/null | sed -E 's/^tuxoffice-(.*)-x86_64\.tar\.xz$/\1/' | sort -V | tail -1)
    [[ -n $TUX_VERSION ]] || die "no TuxOffice release files in $FROM"
    return
  fi
  msg "looking up the latest TuxOffice release"
  TUX_VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" |
            sed -nE 's/^[[:space:]]*"tag_name":[[:space:]]*"v?([^"]+)".*/\1/p' | head -1) || TUX_VERSION=""
  [[ -n $TUX_VERSION ]] || die "could not find a TuxOffice release at github.com/$REPO"
}

fetch_release_file() { # name -> path, verified against SHA256SUMS
  local name=$1 sums
  if [[ -n $FROM ]]; then
    [[ -f $FROM/$name ]] || die "$name is not in $FROM"
    cp "$FROM/$name" "$WORK/$name"
    [[ -f $FROM/SHA256SUMS ]] && cp "$FROM/SHA256SUMS" "$WORK/SHA256SUMS"
  else
    local base="https://github.com/$REPO/releases/download/v$TUX_VERSION"
    msg "downloading $name"
    curl -fL --retry 3 --progress-bar -o "$WORK/$name" "$base/$name" || die "download failed: $base/$name"
    curl -fsSL --retry 3 -o "$WORK/SHA256SUMS" "$base/SHA256SUMS" || die "could not download the checksums"
  fi
  if [[ -f $WORK/SHA256SUMS ]]; then
    sums=$(grep -E "  $name\$" "$WORK/SHA256SUMS") || die "$name is not listed in SHA256SUMS"
    (cd "$WORK" && sha256sum -c --quiet <<<"$sums") || die "checksum mismatch for $name"
    ok "verified $name"
  else
    warn "no SHA256SUMS next to $name; not verified"
  fi
  echo "$WORK/$name" >"$WORK/.fetched"
}

install_system() {
  local file
  fetch_release_file "$(asset_name "$FAMILY")"
  file=$(cat "$WORK/.fetched")
  msg "installing with $(package_tool) (you may be asked for your password)"
  case $FAMILY in
    deb) sudo_cmd apt-get update -qq || true
         sudo_cmd apt-get install -y "$file" ;;
    rpm-dnf) sudo_cmd dnf install -y "$file" ;;
    rpm-zypper) sudo_cmd zypper --non-interactive install --allow-unsigned-rpm "$file" ;;
    arch) sudo_cmd pacman -U --noconfirm --needed "$file" ;;
  esac
  TUXOFFICE=$(command -v tuxoffice || echo /usr/bin/tuxoffice)
}

# What the runner and launcher need from the system, as "tool:NAME" or
# "lib:SONAME", with the package that provides each on deb and Arch systems
# (dnf and zypper install by file or library name directly).
REQUIREMENTS=(
  "tool:python3|python3|python" "tool:7z|7zip|7zip" "tool:unzip|unzip|unzip"
  "tool:cabextract|cabextract|cabextract"
  "lib:libunwind.so.8|libunwind8|libunwind" "lib:libvulkan.so.1|libvulkan1|vulkan-icd-loader"
  "lib:libwayland-client.so.0|libwayland-client0|wayland" "lib:libwayland-egl.so.1|libwayland-egl1|wayland"
  "lib:libxkbcommon.so.0|libxkbcommon0|libxkbcommon" "lib:libxkbregistry.so.0|libxkbregistry0|libxkbcommon"
  "lib:libEGL.so.1|libegl1|libglvnd" "lib:libfreetype.so.6|libfreetype6|freetype2"
  "lib:libfontconfig.so.1|libfontconfig1|fontconfig" "lib:libgnutls.so.30|libgnutls30t64|gnutls"
  "lib:libsecret-1.so.0|libsecret-1-0|libsecret" "lib:libdbus-1.so.3|libdbus-1-3|dbus"
)

have_library() { # soname
  local dir
  if command -v ldconfig >/dev/null || [[ -x /sbin/ldconfig ]]; then
    "$(command -v ldconfig || echo /sbin/ldconfig)" -p 2>/dev/null | grep -q " $1 " && return 0
  fi
  for dir in /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu /lib64 /lib/x86_64-linux-gnu; do
    [[ -e $dir/$1 ]] && return 0
  done
  return 1
}

missing_requirements() { # -> lines "kind:name|deb|arch" that are missing
  local entry what name
  for entry in "${REQUIREMENTS[@]}"; do
    what=${entry%%|*} name=${what#*:}
    case $what in
      tool:*) command -v "$name" >/dev/null || echo "$entry" ;;
      lib:*) have_library "$name" || echo "$entry" ;;
    esac
  done
}

install_requirements() { # entries...
  local entry what name names=()
  for entry in "$@"; do
    what=${entry%%|*} name=${what#*:}
    case $FAMILY in
      deb) names+=("$(cut -d'|' -f2 <<<"$entry")") ;;
      arch) names+=("$(cut -d'|' -f3 <<<"$entry")") ;;
      rpm-dnf|rpm-zypper)
        if [[ $what == tool:* ]]; then names+=("/usr/bin/$name"); else names+=("$name()(64bit)"); fi ;;
    esac
  done
  mapfile -t names < <(printf '%s\n' "${names[@]}" | sort -u)
  case $FAMILY in
    deb) sudo_cmd apt-get install -y "${names[@]}" ;;
    rpm-dnf) sudo_cmd dnf install -y "${names[@]}" ;;
    rpm-zypper) sudo_cmd zypper --non-interactive install "${names[@]}" ;;
    arch) sudo_cmd pacman -S --noconfirm --needed "${names[@]}" ;;
  esac
}

install_user() {
  local file entry missing=() names=()
  command -v xz >/dev/null || die "xz is needed to unpack TuxOffice; install it (xz or xz-utils) and rerun"
  fetch_release_file "$(asset_name user)"
  file=$(cat "$WORK/.fetched")
  msg "installing into $USER_ROOT"
  rm -rf "$USER_ROOT.new"
  mkdir -p "$USER_ROOT.new" "$USER_BIN"
  tar -C "$USER_ROOT.new" -xJf "$file"
  rm -rf "$USER_ROOT" && mv "$USER_ROOT.new" "$USER_ROOT"
  ln -sfn "$USER_ROOT/usr/share/tuxoffice/tuxoffice" "$USER_BIN/tuxoffice"
  TUXOFFICE="$USER_BIN/tuxoffice"

  mapfile -t missing < <(missing_requirements)
  if ((${#missing[@]})); then
    for entry in "${missing[@]}"; do names+=("$(cut -d'|' -f1 <<<"$entry" | cut -d: -f2)"); done
    warn "TuxOffice needs these from your system: ${names[*]}"
    # Without a terminal sudo could not ask for a password: only report.
    if [[ $FAMILY =~ ^(deb|rpm-dnf|rpm-zypper|arch)$ && ( -n $TTY || $ASSUME_YES == 1 ) ]] &&
       [[ $(ask "Install them with $(package_tool) now? [Y/n] " y) =~ ^[Yy] ]]; then
      install_requirements "${missing[@]}" ||
        warn "could not install them; install them with your package manager before running tuxoffice setup"
    else
      warn "install them with your package manager before running tuxoffice setup"
    fi
  fi
  case ":$PATH:" in
    *":$USER_BIN:"*) ;;
    *) warn "$USER_BIN is not on your PATH; add it to your shell's PATH to type just \"tuxoffice\"" ;;
  esac
}

uninstall() {
  if [[ -L $USER_BIN/tuxoffice || -d $USER_ROOT ]]; then
    rm -rf "$USER_ROOT" "$USER_BIN/tuxoffice"
    ok "removed the per-user installation"
  fi
  if [[ -x /usr/share/tuxoffice/tuxoffice ]]; then
    detect_family
    case $FAMILY in
      deb) sudo_cmd apt-get remove -y tuxoffice ;;
      rpm-dnf) sudo_cmd dnf remove -y tuxoffice ;;
      rpm-zypper) sudo_cmd zypper --non-interactive remove tuxoffice ;;
      arch) sudo_cmd pacman -R --noconfirm tuxoffice ;;
      *) die "remove the tuxoffice package with your package manager" ;;
    esac
    ok "removed the tuxoffice package"
  fi
  cat <<EOF

Your Office installation, settings and documents were kept in
  ${XDG_DATA_HOME:-$HOME/.local/share}/tuxoffice
Delete that folder to remove Office completely, and run "tuxoffice unintegrate"
first (or delete ~/.local/share/applications/tuxoffice-*.desktop) to remove the
menu entries.
EOF
}

main() {
  METHOD="" NEXT="" TUX_VERSION="" FROM="" ASSUME_YES=0 DO_UNINSTALL=0 DISTRO_NAME="" FAMILY=""
  while (($#)); do
    case $1 in
      --system) METHOD=system ;;
      --user) METHOD=user ;;
      --office) NEXT=office ;;
      --setup-only) NEXT=setup ;;
      --no-setup) NEXT=none ;;
      --version) TUX_VERSION=${2:?--version needs a version}; TUX_VERSION=${TUX_VERSION#v}; shift ;;
      --from) FROM=$(realpath "${2:?--from needs a directory}"); shift ;;
      --uninstall) DO_UNINSTALL=1 ;;
      -y|--yes) ASSUME_YES=1 ;;
      -h|--help) usage; exit 0 ;;
      *) usage >&2; die "unknown option: $1" ;;
    esac
    shift
  done

  printf '\n%s  TuxOffice%s %s— Microsoft 365 on Linux%s\n\n' "$B" "$R" "$DIM" "$R"
  if ((DO_UNINSTALL)); then uninstall; exit 0; fi

  check_system
  detect_family
  WORK=$(mktemp -d)
  trap 'rm -rf "$WORK"' EXIT

  if [[ -z $METHOD ]]; then
    case $FAMILY in
      deb|rpm-dnf|rpm-zypper|arch)
        case $(choose "How should TuxOffice be installed on ${DISTRO_NAME:-this system}?" 1 \
               "System-wide with $(package_tool) (recommended; updates with your system)" \
               "Just for me, in my home folder (no administrator rights needed)") in
          1) METHOD=system ;; 2) METHOD=user ;;
        esac ;;
      immutable) msg "immutable system detected (${DISTRO_NAME:-ostree}); installing into your home folder"; METHOD=user ;;
      *) msg "no TuxOffice package for ${DISTRO_NAME:-this distribution}; installing into your home folder"; METHOD=user ;;
    esac
  fi
  [[ $METHOD == system && ! $FAMILY =~ ^(deb|rpm-dnf|rpm-zypper|arch)$ ]] &&
    die "no TuxOffice package for ${DISTRO_NAME:-this distribution}; use --user"

  resolve_version
  msg "TuxOffice $TUX_VERSION"
  if [[ $METHOD == system ]]; then install_system; else install_user; fi
  ok "TuxOffice $("$TUXOFFICE" --version | awk '{print $2}') is installed"

  # Unattended runs prepare TuxOffice but never start the 4 GB Office download.
  [[ -z $NEXT && ( $ASSUME_YES == 1 || -z $TTY ) ]] && NEXT=setup
  if [[ -z $NEXT ]]; then
    case $(choose "What next?" 1 \
           "Set up TuxOffice and install Microsoft 365 now (downloads about 4 GB, 15-30 minutes)" \
           "Set up TuxOffice now, install Microsoft 365 later" \
           "Nothing for now") in
      1) NEXT=office ;; 2) NEXT=setup ;; 3) NEXT=none ;;
    esac
  fi

  if [[ $NEXT == setup || $NEXT == office ]]; then
    msg "setting up TuxOffice (runner, Office environment, fonts)"
    "$TUXOFFICE" setup
  fi
  if [[ $NEXT == office ]]; then
    msg "installing Microsoft 365 Apps"
    "$TUXOFFICE" install
  fi

  # The command to show: plain "tuxoffice" when it is on the PATH.
  local run=tuxoffice
  [[ $(command -v tuxoffice 2>/dev/null) ]] || run=$TUXOFFICE
  printf '\n%sDone.%s ' "$GREEN" "$R"
  case $NEXT in
    office) cat <<EOF
Start an app from your application menu or with:
  $run run word
Sign in with the Microsoft account that holds your Microsoft 365 subscription,
then run "$run fonts cloud" once to fetch Office's fonts.
EOF
      ;;
    setup) cat <<EOF
Install Microsoft 365 when you are ready with:
  $run install
EOF
      ;;
    *) cat <<EOF
Continue with:
  $run setup      # prepare TuxOffice
  $run install    # install Microsoft 365
EOF
      ;;
  esac
}

main "$@"
