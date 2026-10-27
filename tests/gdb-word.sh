#!/usr/bin/env bash
# Launch Word, wait for a trigger file, then dump native stacks of every Word
# thread with gdb. Works with kernel.yama.ptrace_scope=1: this script stays an
# ancestor of Word and exec()s into gdb (same PID), which is allowed to attach.
# Wine's launcher exits after spawning Word, so make this script a child
# subreaper first: the orphaned Word then reparents to us instead of systemd.
# Usage: tests/gdb-word.sh TRIGGER_FILE OUT_FILE [document]
set -euo pipefail
if [[ -z ${TUX_SUBREAPER:-} ]]; then
  exec env TUX_SUBREAPER=1 python3 -I -c '
import ctypes, os, sys
ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER
os.execvp("bash", ["bash"] + sys.argv[1:])' "$0" "$@"
fi
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
trigger=$1 out=$2 doc=${3:-}
rm -f "$trigger"
app=${TUX_GDB_APP:-word}
case $app in word) exe=WINWORD.EXE;; excel) exe=EXCEL.EXE;; powerpoint) exe=POWERPNT.EXE;; *) exit 2;; esac
"$HERE/../tuxoffice" run "$app" ${doc:+"$doc"} >"$out.wine.log" 2>&1 &
while [[ ! -e $trigger ]]; do sleep 0.1; done
# The subreaper owns this launch; another prefix may have a desktop Word.
pid=$(pgrep -P "$$" -f "^C:.Program Files.*$exe" | head -1)
if [[ -n ${TUX_GDB_COMMANDS:-} ]]; then
  exec gdb -p "$pid" -batch -nx -ex 'set debuginfod enabled off' -ex 'set pagination off' -x "$TUX_GDB_COMMANDS" >"$out" 2>&1
fi
exec gdb -p "$pid" -batch -nx -ex 'set debuginfod enabled off' -ex 'set pagination off' \
  -ex "source $HERE/../wine/tools/gdbunwind.py" -ex 'thread apply all bt 30' >"$out" 2>&1
