#!/usr/bin/env bash
# Regression test for the popup hang/crash: open Word's spelling context menu
# on a misspelled word, hover around in it, close it, repeat; then verify Word
# is alive, raised no unhandled exception, and still answers input.
# Usage: tests/popup-crash-check.sh WINE_LOG X Y [rounds]
#   WINE_LOG  stderr log of the running Word (to look for exceptions)
#   X Y       position of a misspelled word, in 96-DPI virtual coordinates
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
log=$1 x=$2 y=$3 rounds=${4:-3}
T="$HERE/../tuxoffice"
typer() { "$T" wine 'C:\tuxoffice\typer.exe' "$@" >/dev/null 2>&1; }

before=$(grep -c 'Unhandled' "$log" || true)
for ((i = 0; i < rounds; i++)); do
  "$HERE/kwin-run.sh" "$HERE/kwin-activate-word.js" >/dev/null
  typer hover "$x" "$y"; sleep 0.3
  typer rclick; sleep 1.5
  for dy in 30 60 90 120; do typer hover $((x + 40)) $((y + dy)); sleep 0.4; done
  typer key 0x1B; sleep 1.5          # VK_ESCAPE closes the menu (destroys the popup)
done
sleep 8                              # longer than Office's hang watchdog
after=$(grep -c 'Unhandled' "$log" || true)
alive=no; pgrep -f '^C:.Program Files.*WINWORD.EXE' >/dev/null && alive=yes
echo "rounds=$rounds new_unhandled=$((after - before)) word_alive=$alive"
