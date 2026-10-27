#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Open common Office dialogs and report windows the compositor must scale.

Usage (inside tests/isolated-run.py): dialog-audit.py OUTDIR APP DOCUMENT
A dialog is opened by one typer command or a list of them. For each dialog
the script records every new visible window of the Office
process with its DPI awareness (0 unaware, 1 system, 2 per-monitor) and a
window capture. Unaware or system-aware windows are drawn at 96 DPI and
stretched to the 1.25 scale, which blurs their text.
"""
import json
import os
from pathlib import Path
import re
import subprocess as sp
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
app = sys.argv[2]
document = Path(sys.argv[3]).resolve()
assert os.environ.get("TUXOFFICE_PREFIX", "").endswith("/home/prefix"), "Use the isolated prefix"
out.mkdir(parents=True, exist_ok=True)
DIALOGS = {
    "excel": [("format-cells", ["chord", "0x11", "0x31"]), ("find-replace", ["chord", "0x11", "0x48"]),
              ("insert-function", ["chord", "0x10", "0x72"]), ("save-as", ["key", "0x7b"]),
              ("open", ["chord", "0x11", "0x4f"])],
    "word": [("font", ["chord", "0x11", "0x44"]), ("find-replace", ["chord", "0x11", "0x48"]),
             ("paragraph", ["chord", "0x12", "0x48"]), ("save-as", ["key", "0x7b"])],
    # Find and Replace are task panes in current PowerPoint; select the
    # slide's first shape (Esc, Tab) for dialogs that act on a selection.
    "powerpoint": [("font", [["key", "0x1b"], ["key", "0x09"], ["chord", "0x11", "0x54"]]),
                   ("hyperlink", [["key", "0x1b"], ["key", "0x09"], ["chord", "0x11", "0x4b"]]),
                   ("save-as", ["key", "0x7b"])],
}


def typer(*args):
    with tempfile.TemporaryFile(mode="w+") as output:
        sp.run([str(root / "tuxoffice"), "wine", "C:/tuxoffice/typer.exe", "--app", app, *map(str, args)],
               stdout=output, stderr=output, timeout=30)
        output.seek(0)
        return output.read()


def windows():
    rows = []
    for line in typer("dpi-windows").splitlines():
        m = re.match(r"(\S+) awareness=(-?\d+) dpi=(\d+) rect=(-?\d+),(-?\d+),(-?\d+),(-?\d+) class=(.*?) title=(.*)$", line)
        if m:
            rows.append({"hwnd": m[1], "awareness": int(m[2]), "dpi": int(m[3]),
                         "rect": [int(m[i]) for i in range(4, 8)], "class": m[8], "title": m[9]})
    return rows


sp.run([str(root / "tuxoffice"), "kill"], check=True)
copy = out / f"audit-{app}{document.suffix}"
copy.write_bytes(document.read_bytes())
office = sp.Popen([str(root / "tuxoffice"), "run", app, str(copy)], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
deadline = time.monotonic() + 120
while not any(copy.stem in w["title"] for w in windows()):
    assert time.monotonic() < deadline, "The document window did not appear"
    time.sleep(1)
time.sleep(5)
results = []
for name, command in DIALOGS[app]:
    sp.run([sys.executable, str(root / "tests/activate-office.py"), app, copy.stem], check=True)
    before = {w["hwnd"] for w in windows()}
    for step in command if isinstance(command[0], list) else [command]:
        typer(*step)
        time.sleep(0.5)
    deadline = time.monotonic() + 10
    new = []
    while time.monotonic() < deadline:
        time.sleep(0.5)
        new = [w for w in windows() if w["hwnd"] not in before]
        if new:
            time.sleep(1.5)
            new = [w for w in windows() if w["hwnd"] not in before]
            break
    entry = {"dialog": name, "windows": new,
             "scaled": [w for w in new if w["awareness"] != 2]}
    if new:
        sp.run([str(root / "tests/capture-word.py"), str(out / f"{name}.png"), app], capture_output=True)
    results.append(entry)
    print(json.dumps(entry), flush=True)
    for _ in range(3):
        if not [w for w in windows() if w["hwnd"] not in before]:
            break
        typer("key", "0x1b")
        time.sleep(1)
summary = {"app": app, "dialogs": len(results), "opened": sum(1 for r in results if r["windows"]),
           "scaled_windows": sum(len(r["scaled"]) for r in results)}
(out / "summary.json").write_text(json.dumps({"summary": summary, "results": results}, indent=1) + "\n")
print(json.dumps(summary))
sp.run([str(root / "tuxoffice"), "kill"])
office.wait(timeout=30)
