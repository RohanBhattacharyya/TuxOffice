#!/usr/bin/env bash
# Apply a suggestion in the rendering test document and verify a saved DOCX.
# Run through isolated-run.py; never use a document with unsaved user work.
# Usage: tests/spelling-functional.sh OUTDIR
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T="$HERE/../tuxoffice"
out=$(realpath -m "${1:?output directory}")
mkdir -p "$out"
[[ ${TUXOFFICE_PREFIX:-} == */home/prefix ]] || {
  echo 'Run this test in an isolated-session.sh prefix' >&2; exit 2;
}
typer() { "$T" wine 'C:\tuxoffice\typer.exe' "$@"; }
probe() { "$T" wine 'C:\tuxoffice\office-probe.exe' "$@"; }

"$HERE/hang-hunt.sh" "$out/open" spell-click 2
probe word-text > "$out/before.txt"
read -r x y < <(probe word-click-point 2>"$out/point.log" | tr -d '\r')
[[ $x =~ ^[0-9]+$ && $y =~ ^[0-9]+$ ]]
"$HERE/activate-office.py" word
typer key 0x1B
typer hover "$x" "$y"; sleep 0.4; typer lclick; sleep 1
typer hover $((x + 30)) $((y + 70)); sleep 0.3
"$HERE/capture-word.py" "$out/card.png"
typer lclick; sleep 0.5
probe word-text > "$out/after.txt"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
before = (p / 'before.txt').read_text()
after = (p / 'after.txt').read_text()
assert 'consectetur' in before, 'Test word absent before clicking'
assert 'consectetur' not in after, 'Click did not apply a suggestion'
assert 'nonsecretor' in after, 'Unexpected replacement for the pinned Office test document'
PY
# A fresh filename avoids Office's overwrite dialog when repeating the test.
savedir=$(mktemp -d "$out/save.XXXXXX")
winpath=$("$T" wine winepath -w "$savedir/result.docx" 2>/dev/null | tr -d '\r')
probe word-save-as "$winpath"
python3 - "$savedir/result.docx" <<'PY'
import sys
import xml.etree.ElementTree as ET
import zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    text = ''.join(ET.fromstring(archive.read('word/document.xml')).itertext())
assert 'nonsecretor' in text and 'consectetur' not in text, text
print('Suggestion applied and saved DOCX verified:', sys.argv[1])
PY
