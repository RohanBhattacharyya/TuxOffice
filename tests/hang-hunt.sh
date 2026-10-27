#!/usr/bin/env bash
# Catch Word hangs with native stacks. Launches Word under tests/gdb-word.sh,
# waits until its UI thread answers, runs a scenario, then pings the UI thread
# every 300 ms; on the first missed ping (or an unhandled exception) gdb dumps
# every thread before Office's hang watchdog kills the process.
# Usage: tests/hang-hunt.sh OUTDIR SCENARIO [watch-seconds]
# Scenarios: spell-click (left-click a misspelled word: Editor suggestion card)
#            spell-menu  (right-click it: context menu with suggestions)
#            soak        (typing, ribbon tabs, menus, scrolling, popups)
#            resize      (resize and maximize without opening a popup)
#            resize-popup (open suggestions between resize/maximize cycles)
#            popup-cycle (20 open/close cycles of the spelling card)
#            resize-live-popup (resize/maximize while the card is open)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out=${1:?usage: hang-hunt.sh OUTDIR SCENARIO [seconds]}
scenario=${2:?scenario}
watch=${3:-25}
T="$HERE/../tuxoffice"
DOC="${TUX_TEST_DOC:-$HOME/Documents/TuxOffice-test/render-test.docx}"
# Position of a misspelled word ("consectetur") in 96-DPI virtual coordinates.
WX=${TUX_WORD_X:-983} WY=${TUX_WORD_Y:-449}
mkdir -p "$out"; rm -f "$out"/{trigger,gdb.txt,gdb.txt.wine.log,summary.txt,startup.log}
phase=startup
finish() {
  status=$?
  touch "$out/trigger"
  if [[ ! -f $out/summary.txt ]]; then
    printf 'scenario=%s verdict=failed phase=%s exit_status=%s\nnative_stacks=%s/gdb.txt\n' \
      "$scenario" "$phase" "$status" "$out" > "$out/summary.txt"
  fi
}
trap finish EXIT
typer() { "$T" wine 'C:\tuxoffice\typer.exe' "$@" 2>/dev/null | sed '/^ntsync:/d;s/\r$//'; }
activate() { "$HERE/activate-office.py" word "$(basename "${DOC%.*}")"; sleep 0.3; typer raise >/dev/null; }
open_card() {
  local point attempt
  typer key 0x1B
  point=$(timeout 20 "$T" wine 'C:\tuxoffice\office-probe.exe' word-click-point 2>>"$out/point.log" | tr -d '\r')
  read -r WX WY <<<"$point"
  [[ $WX =~ ^[0-9]+$ && $WY =~ ^[0-9]+$ ]] || return 1
  typer hover "$WX" "$WY"
  typer hit-test | grep -F "$(basename "${DOC%.*}")" >/dev/null || return 1
  typer lclick
  for attempt in $(seq 1 12); do
    if typer popup-rect >>"$out/card-rects.log"; then return 0; fi
    sleep 0.05
  done
  echo 'Spelling card did not appear' >&2
  return 1
}

# Avoid leaving Office's recovery/session restore state behind on every run.
timeout 10 "$T" wine 'C:\tuxoffice\office-probe.exe' word-quit >/dev/null 2>&1 || true
"$T" kill; sleep 1
WINEDEBUG="${WINEDEBUG:-+timestamp,warn+seh,err+all,fixme-all,-d3d}" setsid "$HERE/gdb-word.sh" "$out/trigger" "$out/gdb.txt" "$DOC" &

for _ in $(seq 1 "${TUX_STARTUP_ATTEMPTS:-90}"); do
  sleep 2
  typer dismiss-startup
  r=$(typer ping 1500 || true)
  printf '%s %s\n' "$SECONDS" "$r" >> "$out/startup.log"
  [[ $r == ok* ]] && break
done
[[ "$(typer ping 1500)" == ok* ]] || { echo 'Word did not become ready' >&2; exit 1; }
sleep 8   # let startup work settle
if [[ $scenario != resize && -z ${TUX_WORD_X:-} && -z ${TUX_WORD_Y:-} ]]; then
  point=$(timeout 20 "$T" wine 'C:\tuxoffice\office-probe.exe' word-click-point 2>"$out/point.log" | tr -d '\r')
  read -r WX WY <<<"$point"
  [[ $WX =~ ^[0-9]+$ && $WY =~ ^[0-9]+$ ]] || { echo "Unable to locate test word" >&2; exit 1; }
fi
echo ":: Word up; scenario $scenario"

run_scenario() {
  case $scenario in
    spell-click)
      activate; typer hover "$WX" "$WY"; sleep 0.4; typer lclick; sleep 1.5
      typer hover $((WX + 30)) $((WY + 40)); sleep 1; typer hover $((WX + 30)) $((WY + 70)); sleep 1 ;;
    spell-menu)
      activate; typer hover "$WX" "$WY"; sleep 0.4; typer rclick; sleep 1.5
      for dy in 30 60 90; do typer hover $((WX + 40)) $((WY + dy)); sleep 0.5; done
      typer key 0x1B ;;
    popup-cycle)
      activate
      typer popup-bench "$WX" "$WY" 20
      typer key 0x1B ;;
    resize|resize-popup|resize-live-popup)
      activate
      for size in '1100 750' '1500 950' '900 650' '1700 1000'; do
        read -r width height <<<"$size"
        if [[ $scenario == resize-live-popup ]]; then open_card; fi
        typer resize "$width" "$height"
        sleep 2
        if [[ $scenario == resize-live-popup ]]; then
          open_card
          "$HERE/capture-word.py" "$out/restored-card-$width.png" word --active >/dev/null
          python3 "$HERE/check-floating-strips.py" >>"$out/window-checks.log"
          python3 - "$out/restored-card-$width.png" <<'PY'
from PIL import Image
import sys
with Image.open(sys.argv[1]) as image:
    assert min(image.size) >= 300, f"A helper window took focus: {image.size}"
PY
        fi
        typer maximize
        sleep 2
        if [[ $scenario == resize-popup || $scenario == resize-live-popup ]]; then
          point=$(timeout 20 "$T" wine 'C:\tuxoffice\office-probe.exe' word-click-point 2>>"$out/point.log" | tr -d '\r')
          read -r WX WY <<<"$point"
          [[ $WX =~ ^[0-9]+$ && $WY =~ ^[0-9]+$ ]] || return 1
          typer hover "$WX" "$WY"; sleep 0.4; typer lclick; sleep 1
          typer hover $((WX + 30)) $((WY + 70)); sleep 0.5
          typer key 0x1B
        fi
      done ;;
    soak)
      for round in 1 2 3; do
        activate; typer key 0x23; typer type " soak round $round with misspeled wrods " 25; sleep 0.5
        typer hover "$WX" "$WY"; typer lclick; sleep 1.2; typer key 0x1B; sleep 0.3
        typer hover "$WX" "$WY"; typer rclick; sleep 1.2; typer key 0x1B; sleep 0.3
        for tab in 0x12 0x4E 0x12 0x50 0x12 0x52 0x12 0x48; do typer key "$tab"; sleep 0.3; done  # Alt / Alt shortcuts walk ribbon tabs
        typer key 0x1B; typer key 0x1B
        typer wheel -5; sleep 0.4; typer wheel 5; sleep 0.4
      done ;;
    *) echo "unknown scenario $scenario" >&2; exit 1 ;;
  esac
}
phase=scenario
run_scenario

phase=watch
verdict=ok
end=$((SECONDS + watch))
while ((SECONDS < end)); do
  r=$(typer ping 700 || true)
  if [[ $r != ok* ]] || grep -q 'Unhandled' "$out/gdb.txt.wine.log" 2>/dev/null; then
    verdict="hang-or-crash ($r)"; touch "$out/trigger"; break
  fi
  sleep 0.3
done
if [[ $verdict == ok ]]; then
  if ! "$HERE/check-render-faults.py" "$out/gdb.txt.wine.log"; then
    verdict=graphics-access-violation
  fi
  touch "$out/trigger"   # release the launcher; gdb then just samples a healthy Word
fi
for _ in $(seq 1 90); do grep -q 'detached\]' "$out/gdb.txt" 2>/dev/null && break; sleep 2; done
{
  echo "scenario=$scenario verdict=$verdict"
  echo "unhandled=$(grep -c 'Unhandled' "$out/gdb.txt.wine.log" 2>/dev/null || true)"
  echo "native_stacks=$out/gdb.txt"
} | tee "$out/summary.txt"
[[ $verdict == ok ]]
