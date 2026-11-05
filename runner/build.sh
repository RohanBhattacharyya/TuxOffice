#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the TuxOffice runner from wine/ (Valve's Proton Wine + Soda's Office
# patches, imported in the commit whose subject starts with "soda", followed
# by TuxOffice's own Wine commits).
#
# Usage: runner/build.sh [--full]
#
# Default (overlay): rebuild only the modules TuxOffice changed relative to
# the "soda" commit (committed or not) and drop them into a copy of the
# pinned Soda release. A minute instead of an hour, and the result keeps the
# exact Soda binaries for everything else.
# --full: configure and build the whole tree into the runner prefix.
#
# The build directory is out of tree (${TUXOFFICE_BUILD_DIR}/build), so the
# source repo stays clean and incremental builds survive branch switches.
# Needs: clang, lld, bison, flex, and Wine's -dev libraries (PE modules are
# built with clang via --with-mingw=clang, no mingw-gcc).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/../wine" && pwd)"
TUX_HOME="${TUXOFFICE_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/tuxoffice}"
WORK="${TUXOFFICE_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/tuxoffice-build}"
BUILD="$WORK/build"
FULL=0
[[ ${1:-} == --full ]] && FULL=1

# The Soda release our base matches; its binaries back the overlay build.
SODA_RUNNER="soda-11.0-33-experimental-x86_64"
OUT="$TUX_HOME/runners/tuxoffice-${SODA_RUNNER#soda-}"

mkdir -p "$BUILD"
cd "$BUILD"
if [[ ! -f Makefile || "$SRC/configure" -nt Makefile ]]; then
  if ((FULL)); then
    "$SRC/configure" -q --prefix="$OUT" --enable-archs=i386,x86_64 --with-mingw=clang \
      --disable-tests --with-wayland --with-vulkan
  else
    "$SRC/configure" -q --enable-archs=i386,x86_64 --with-mingw=clang --disable-tests >/dev/null
  fi
fi

install_support_files() {
  local destination=$1
  make -j"$(nproc)" dlls/winspool.drv/x86_64-windows/libwinspool.a >/dev/null
  tools/winegcc/winegcc --wine-objdir "$BUILD" -b x86_64-windows -mconsole \
    -I"$SRC/include" -I"$SRC/include/msvcrt" -L"$BUILD/dlls/winspool.drv/x86_64-windows" \
    -o "$WORK/pdf-printer.exe" "$HERE/../lib/pdf-printer.c" \
    -lwinspool -lwinecrt0 -lucrtbase -lkernel32 -lntdll
  tools/winegcc/winegcc --wine-objdir "$BUILD" -b x86_64-windows -mconsole \
    -I"$SRC/include" -I"$SRC/include/msvcrt" \
    -o "$WORK/office-ui.exe" "$HERE/../lib/office-ui.c" \
    -luser32 -lgdi32 -lwinecrt0 -lucrtbase -lkernel32 -lntdll
  make -j"$(nproc)" dlls/crypt32/x86_64-windows/libcrypt32.a >/dev/null
  tools/winegcc/winegcc --wine-objdir "$BUILD" -b x86_64-windows -mconsole \
    -I"$SRC/include" -I"$SRC/include/msvcrt" -L"$BUILD/dlls/crypt32/x86_64-windows" \
    -o "$WORK/root-certs.exe" "$HERE/../lib/root-certs.c" \
    -lcrypt32 -lwinecrt0 -lucrtbase -lkernel32 -lntdll
  mkdir -p "$destination/share/tuxoffice" "$destination/bin"
  # Native helper that shows the desktop's file chooser for Office's dialogs.
  if pkg-config --exists libsystemd; then
    cc -O2 -Wall -o "$destination/bin/tuxoffice-file-dialog" "$HERE/../lib/file-dialog.c" \
      $(pkg-config --cflags --libs libsystemd)
  else
    echo 'warning: libsystemd not found; Office will use Wine file dialogs' >&2
  fi
  install -m 0644 "$WORK/pdf-printer.exe" "$destination/share/tuxoffice/pdf-printer.exe"
  install -m 0644 "$WORK/office-ui.exe" "$destination/share/tuxoffice/office-ui.exe"
  install -m 0644 "$WORK/root-certs.exe" "$destination/share/tuxoffice/root-certs.exe"
  install -m 0644 "$SRC/dlls/winspool.drv/generic.ppd" "$destination/share/tuxoffice/layout.ppd"
  printf '%s\n' 'Direct2D effect draw-info ordering and composition commit notifications' \
    > "$destination/share/tuxoffice/office-graphics-v1"
  if [[ ${TUXOFFICE_SKIP_DXVK_BUILD:-0} != 1 ]]; then
    if ! "$HERE/build-dxvk.sh" "$destination/share/tuxoffice/dxvk"; then
      echo 'warning: DXVK build failed; automatic graphics will use built-in D3D' >&2
    fi
  fi
}

build_unix32_ntdll() {
  # Soda has a legacy 32-bit Unix loader as well as its 64-bit one. Rebuild
  # that client's protocol handshake too, even though Office itself is x64.
  local build32="$WORK/build32"
  mkdir -p "$build32"
  if [[ ! -f "$build32/Makefile" || "$SRC/configure" -nt "$build32/Makefile" ]]; then
    (cd "$build32" && CC='gcc -m32' CXX='g++ -m32' "$SRC/configure" -q \
      --with-wine64="$BUILD" --with-mingw=clang --disable-tests \
      --without-x --without-wayland --without-vulkan)
  fi
  make -C "$build32" -j"$(nproc)" dlls/ntdll/all
}

install_unix32_ntdll() {
  local destination=$1
  mkdir -p "$destination/lib/wine/i386-unix"
  install -m0755 "$WORK/build32/dlls/ntdll/ntdll.so" "$destination/lib/wine/i386-unix/ntdll.so"
}

if ((FULL)); then
  make -j"$(nproc)"
  build_unix32_ntdll
  make install
  install_unix32_ntdll "$OUT"
  install_support_files "$OUT"
  echo ":: runner installed to $OUT"
  exit 0
fi

base=$(git -C "$SRC" log --format=%H --grep='^soda' -n1 -- .)
[[ -n $base ]] || { echo "no 'soda' commit in $SRC" >&2; exit 1; }
# wine/ is part of the TuxOffice repository; --relative keeps paths inside it.
# A new module is untracked until committed, and git diff does not list it.
mapfile -t dlls < <( { git -C "$SRC" diff --relative --name-only "$base" -- dlls; git -C "$SRC" diff --relative --name-only -- dlls;
                       git -C "$SRC" ls-files --others --exclude-standard -- dlls; } |
                     cut -d/ -f2 | grep -v '^Makefile' | sort -u)
# The generated server protocol version also lives in ntdll's Unix client.
# Shared-memory layout changes must ship a matching client and server.
protocol_changed=0
if ! git -C "$SRC" diff --quiet "$base" -- server/protocol.def; then
  dlls+=(ntdll)
  protocol_changed=1
fi
server_changed=0
git -C "$SRC" diff --quiet "$base" -- server || server_changed=1
(( ${#dlls[@]} || server_changed )) || { echo "no modules changed since the soda commit" >&2; exit 1; }
echo ":: overlay build for: ${dlls[*]}"
make -j"$(nproc)" __tooldeps__ >/dev/null
for d in "${dlls[@]}"; do make -j"$(nproc)" "dlls/$d/all"; done
if ((server_changed)); then make -j"$(nproc)" server/wineserver; fi
if ((protocol_changed)); then build_unix32_ntdll; fi

base_runner="$TUX_HOME/runners/$SODA_RUNNER"
[[ -x $base_runner/bin/wine ]] || { echo "base runner $base_runner missing; run tuxoffice setup first" >&2; exit 1; }
rm -rf "$OUT.tmp"
# A staging home may link to a shared base runner. Copy its directory, not
# the link: installing into a copied symlink would overwrite the base itself.
cp -a --reflink=auto "$(readlink -f "$base_runner")" "$OUT.tmp"
install_support_files "$OUT.tmp"
if ((server_changed)); then install -m 0755 server/wineserver "$OUT.tmp/bin/wineserver"; fi
if ((protocol_changed)); then install_unix32_ntdll "$OUT.tmp"; fi
missing=()
for d in "${dlls[@]}"; do
  # configure disables a module whose libraries are missing; shipping Soda's
  # unpatched copy instead would go unnoticed, so stop.
  compgen -G "dlls/$d/x86_64-windows/*.[de][lrx][lve]" >/dev/null || missing+=("$d")
  for arch in x86_64 i386; do
    pe=$(ls "dlls/$d/$arch-windows/"*.{dll,drv,exe} 2>/dev/null | head -1) || true
    [[ -n $pe ]] && install -m 0644 "$pe" "$OUT.tmp/lib/wine/$arch-windows/$(basename "$pe")"
  done
  # Unix halves are named after the module (winewayland.drv -> winewayland.so).
  for so in "dlls/$d"/*.so; do
    [[ -f $so ]] && install -m 0755 "$so" "$OUT.tmp/lib/wine/x86_64-unix/$(basename "$so")"
  done
done
if ((${#missing[@]})); then
  echo "error: changed modules were not built (missing build dependencies?): ${missing[*]}" >&2
  rm -rf "$OUT.tmp"; exit 1
fi
# Wine only puts a builtin module into a prefix's system32 when it updates the prefix, which
# it does when wine.inf's time differs from the one the prefix recorded. A module the runner
# being replaced did not have would otherwise never be found by existing prefixes, so give
# wine.inf a new time then, and otherwise keep the installed runner's, so that a rebuild does
# not update every prefix (the launcher sets Office up again after an update).
previous="$OUT/lib/wine/x86_64-windows"
[[ -d $previous ]] || previous="$base_runner/lib/wine/x86_64-windows"
# A module's COM classes and other registry entries are its WINE_REGISTRY resources, which
# prefixes also only apply when they update. Compare those, without their addresses, which
# move whenever the code does.
registration() {
  command -v llvm-readobj >/dev/null || return 0
  llvm-readobj --coff-resources "$1" 2>/dev/null | sed -n '/Type: WINE_REGISTRY/,$p' | grep -v 'DataRVA'
}
new_module= new_registration=
for d in "${dlls[@]}"; do
  for pe in "dlls/$d/x86_64-windows/"*.{dll,drv,exe}; do
    [[ -f $pe ]] || continue
    if [[ ! -e "$previous/$(basename "$pe")" ]]; then new_module=$(basename "$pe"); break 2; fi
    [[ -z $new_registration && "$(registration "$pe")" != "$(registration "$previous/$(basename "$pe")")" ]] &&
      new_registration=$(basename "$pe")
  done
done
if [[ -n $new_module || -n $new_registration ]]; then
  if [[ -n $new_module ]]; then echo ":: $new_module is new; existing prefixes will update on their next start"
  else echo ":: $new_registration registers new entries; existing prefixes will update on their next start"; fi
  touch "$OUT.tmp/share/wine/wine.inf"
elif [[ -f "$OUT/share/wine/wine.inf" ]]; then
  touch -r "$OUT/share/wine/wine.inf" "$OUT.tmp/share/wine/wine.inf"
fi
if [[ " ${dlls[*]} " == *" winewayland.drv "* && ! -f dlls/winewayland.drv/winewayland.so ]]; then
  echo "error: winewayland.so was not built; install the Wayland and xkbcommon development files" >&2
  rm -rf "$OUT.tmp"; exit 1
fi
# Keep Soda's separate debug info next to the libraries for gdb.
[[ -d $OUT/lib/wine/x86_64-unix/.debug ]] && cp -a "$OUT/lib/wine/x86_64-unix/.debug" "$OUT.tmp/lib/wine/x86_64-unix/" 2>/dev/null || true
rm -rf "$OUT" && mv "$OUT.tmp" "$OUT"
echo ":: runner installed to $OUT"
