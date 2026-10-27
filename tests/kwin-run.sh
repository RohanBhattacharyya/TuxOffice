#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Run a one-off KWin script on the live Plasma session and print its output.
# Usage: tests/kwin-run.sh SCRIPT.js
# The script is loaded over D-Bus and unloaded again; nothing is written to
# the user's KWin configuration. Output lines come from print() in the script
# and are read back from the user journal (lines must start with "TUX").
set -euo pipefail
script=$(readlink -f "$1")
name="tuxoffice-$$"
since=$(date '+%Y-%m-%d %H:%M:%S')
offset=1
[[ -z ${TUX_KWIN_LOG:-} ]] || offset=$(( $(wc -c <"$TUX_KWIN_LOG") + 1 ))
id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$script" "$name")
qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null
sleep 0.3
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$name" >/dev/null || true
if [[ -n ${TUX_KWIN_LOG:-} ]]; then
  output=$(tail -c +"$offset" "$TUX_KWIN_LOG" | sed -n 's/^.*\(TUX[A-Z]* .*\)$/\1/p')
  if [[ -n $output ]]; then
    printf '%s\n' "$output"
  elif [[ -n ${TUX_KWIN_PID:-} ]]; then
    journalctl --user _PID="$TUX_KWIN_PID" --since "$since" -o cat 2>/dev/null | grep -a '^TUX' || true
  fi
else
  journalctl --user --since "$since" -o cat 2>/dev/null | grep -a '^TUX' || true
fi
