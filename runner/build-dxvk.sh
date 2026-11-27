#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the pinned DXVK release with the immediate-context state-swap lock.
# Sources, compiler and incremental build files stay outside the repository.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${TUXOFFICE_DXVK_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/tuxoffice-dxvk}"
DESTINATION=${1:?destination directory}
REVISION=b1a1c99ab52b687cf950d62c88bc2fa316b41663
TOOLCHAIN=llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64
TOOLCHAIN_SHA256=55064b60b991a1e1e8090b8c2f84e82b22cfe5903d12c159d65064cf5e8ae335
for program in git curl tar python3 meson ninja glslangValidator; do
  command -v "$program" >/dev/null || { echo "missing DXVK build tool: $program" >&2; exit 1; }
done
mkdir -p "$WORK/toolchain" "$DESTINATION"
if [[ ! -x "$WORK/toolchain/$TOOLCHAIN/bin/x86_64-w64-mingw32-clang++" ]]; then
  if [[ ! -s "$WORK/toolchain/llvm-mingw.tar.xz" ]]; then
    curl -fsSL --retry 3 "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/$TOOLCHAIN.tar.xz" \
      -o "$WORK/toolchain/llvm-mingw.tar.xz.part"
    mv "$WORK/toolchain/llvm-mingw.tar.xz.part" "$WORK/toolchain/llvm-mingw.tar.xz"
  fi
  printf '%s  %s\n' "$TOOLCHAIN_SHA256" "$WORK/toolchain/llvm-mingw.tar.xz" | sha256sum -c --quiet
  tar -C "$WORK/toolchain" -xf "$WORK/toolchain/llvm-mingw.tar.xz"
fi
if [[ ! -d "$WORK/source/.git" ]]; then
  git clone --depth 1 --branch v3.1.1 --recurse-submodules --shallow-submodules \
    https://github.com/doitsujin/dxvk.git "$WORK/source"
fi
[[ $(git -C "$WORK/source" rev-parse HEAD) == "$REVISION" ]] || {
  echo "DXVK source revision differs from the tested revision" >&2; exit 1;
}
git -C "$WORK/source" submodule update --init --recursive --depth 1
# Start from the pristine tree: a patch changed since the last build neither
# applies over its old version nor reverses cleanly.
git -C "$WORK/source" checkout -- .
for patch in "$HERE/patches/dxvk-context-state-lock.patch" "$HERE/patches/dxvk-resource-import-lock.patch" \
    "$HERE/patches/dxvk-composition-present.patch" "$HERE/patches/dxvk-context-state-copy.patch"; do
  git -C "$WORK/source" apply "$patch"
done
python3 - "$WORK" "$WORK/toolchain/$TOOLCHAIN/bin" <<'PY'
from pathlib import Path
import sys
work, tool = map(Path, sys.argv[1:])
for architecture, family in [('x86_64','x86_64'), ('i686','x86')]:
    names = {'c':'clang', 'cpp':'clang++', 'ar':'ar', 'strip':'strip', 'windres':'windres'}
    binaries = '\n'.join(f"{key} = {str(tool / (architecture+'-w64-mingw32-'+name))!r}" for key,name in names.items())
    (work / f'cross-{architecture}.txt').write_text(f"[binaries]\n{binaries}\n\n[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\nsystem = 'windows'\ncpu_family = '{family}'\ncpu = '{architecture}'\nendian = 'little'\n")
PY
for bits in 64 32; do
  if [[ $bits == 64 ]]; then architecture=x86_64; folder=x64; else architecture=i686; folder=x32; fi
  if [[ ! -f "$WORK/build$bits/build.ninja" ]]; then
    meson setup "$WORK/build$bits" "$WORK/source" --cross-file "$WORK/cross-$architecture.txt" \
      --buildtype release -Ddebug=true -Denable_d3d8=false -Denable_d3d9=false
  fi
  ninja -C "$WORK/build$bits" -j"${TUXOFFICE_BUILD_JOBS:-8}"
  mkdir -p "$DESTINATION/$folder"
  for entry in d3d11/d3d11 dxgi/dxgi d3d10/d3d10core; do
    library=${entry#*/}
    install -m0644 "$WORK/build$bits/src/$entry.dll" "$DESTINATION/$folder/$library.dll"
    "$WORK/toolchain/$TOOLCHAIN/bin/$architecture-w64-mingw32-strip" --strip-unneeded \
      "$DESTINATION/$folder/$library.dll"
  done
done
install -m0644 "$WORK/source/LICENSE" "$DESTINATION/LICENSE"
printf '%s\n' "DXVK 3.1.1 $REVISION, synchronized state swaps and resource imports, LLVM-MinGW 20261020" \
  > "$DESTINATION/state-lock-v1"
printf '%s\n' 'Resource imports hold the shared allocation pool mutex' > "$DESTINATION/resource-pool-lock-v1"
printf '%s\n' 'Composition swap chains count presents without a window' > "$DESTINATION/composition-present-v1"
printf '%s\n' 'Context state swaps copy only bound slots' > "$DESTINATION/context-state-copy-v1"
echo ":: patched DXVK installed to $DESTINATION"
