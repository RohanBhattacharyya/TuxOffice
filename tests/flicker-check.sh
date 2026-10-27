#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Self-test for the Word flicker: bring Word to the front, drive typing and
# scrolling over COM, capture the active window repeatedly, and report frames
# whose ribbon or page area turns (near-)white compared to the median frame.
# Usage: tests/flicker-check.sh OUTDIR [seconds]
# Needs: a running Word (tuxoffice run word), KDE Plasma (spectacle, KWin).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out=${1:?usage: flicker-check.sh OUTDIR [seconds]}
secs=${2:-12}
mkdir -p "$out"
rm -f "$out"/frame-*.png

"$HERE/kwin-run.sh" "$HERE/kwin-activate-word.js" >/dev/null
sleep 0.5

prefix_dir="${TUXOFFICE_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/tuxoffice/prefixes/office}"
mkdir -p "$prefix_dir/drive_c/tuxoffice"
cp "$HERE/word-activity.vbs" "$prefix_dir/drive_c/tuxoffice/word-activity.vbs"
"$HERE/../tuxoffice" wine cscript //nologo 'C:\tuxoffice\word-activity.vbs' "$secs" \
  >"$out/activity.log" 2>&1 &
driver=$!

n=0
end=$((SECONDS + secs))
while ((SECONDS < end)); do
  spectacle -b -n -a -o "$(printf '%s/frame-%03d.png' "$out" "$n")" 2>/dev/null || true
  n=$((n + 1))
done
wait "$driver" || true

# Mean brightness of the ribbon band and the page area per frame.
for f in "$out"/frame-*.png; do
  [[ -s $f ]] || continue
  read -r w h < <(magick identify -format '%w %h\n' "$f")
  ribbon=$(magick "$f" -crop "$((w * 8 / 10))x$((h / 12))+$((w / 10))+$((h / 18))" -colorspace gray -format '%[fx:mean]' info:)
  page=$(magick "$f" -crop "$((w / 2))x$((h / 3))+$((w / 4))+$((h / 3))" -colorspace gray -format '%[fx:mean]' info:)
  echo "$(basename "$f") $ribbon $page"
done >"$out/stats.txt"

python3 -I - "$out/stats.txt" <<'EOF'
import statistics, sys
rows = [l.split() for l in open(sys.argv[1]) if l.strip()]
if not rows:
    sys.exit("no frames captured")
ribbon = [float(r[1]) for r in rows]
page = [float(r[2]) for r in rows]
mr, mp = statistics.median(ribbon), statistics.median(page)
bad = [r[0] for r in rows if float(r[1]) > max(0.985, mr + 0.03)]
print(f"frames {len(rows)}  ribbon median {mr:.3f}  page median {mp:.3f}")
print(f"white-flash frames (ribbon washed out): {len(bad)}")
for name in bad[:10]:
    print("  " + name)
EOF
cat "$out/activity.log" | grep -a typed || true
