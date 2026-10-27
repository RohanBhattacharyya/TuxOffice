#!/usr/bin/env bash
# Is Word's text caret visible? Bring Word forward, put the caret at the end
# of the document, then capture frames while idle: the blinking caret is the
# only thing that should change between them.
# Usage: tests/caret-check.sh OUTDIR
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out=${1:?usage: caret-check.sh OUTDIR}
mkdir -p "$out"
rm -f "$out"/caret-*.png
T="$HERE/../tuxoffice"
prefix_dir="${TUXOFFICE_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/tuxoffice/prefixes/office}"
install -m 0755 "${TYPER:-$prefix_dir/drive_c/tuxoffice/typer.exe}" "$prefix_dir/drive_c/tuxoffice/typer.exe" 2>/dev/null || true

"$HERE/kwin-run.sh" "$HERE/kwin-activate-word.js" >/dev/null
sleep 0.5
"$T" wine 'C:\tuxoffice\typer.exe' key 0x23 >/dev/null 2>&1   # VK_END
sleep 1
for i in 0 1 2 3 4 5 6 7; do
  spectacle -b -n -a -o "$out/caret-$i.png" 2>/dev/null
  sleep 0.2
done
changed=0
for i in 1 2 3 4 5 6 7; do
  px=$(magick compare -metric AE "$out/caret-$((i - 1)).png" "$out/caret-$i.png" null: 2>&1 | awk '{print int($1)}')
  echo "frame $((i - 1))->$i changed pixels: $px"
  ((px > 0)) && changed=$((changed + 1))
done
echo "frame pairs with changes: $changed/7 (0 = no visible blinking caret)"
