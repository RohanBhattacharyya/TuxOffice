#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run an app's object-model test in the isolated session, watching for faults.

Usage (inside tests/isolated-run.py): run.py OUTDIR APP [TIMEOUT]
APP is access, publisher, outlook, formats, media, vba or powerquery (the matching APP.vbs runs
under Wine's cscript) or onenote (onenote-api.exe, see tests/build-helpers.sh). While it runs
the script records the visible windows every few seconds, so a modal dialog that
stalls automation names itself. Afterwards it reports the PASS/FAIL lines, the
time measurements and any unhandled exception.

Run Outlook and OneNote without a network, so that they cannot reach a real
mailbox or notebook: unshare --user --map-current-user --net -- run.py ...
Outlook needs a local profile first: import outlook.prf with /importprf, then
choose "Use Outlook without an email account" once. Office must be signed in.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess as sp
import sys
import threading
import time

root = Path(__file__).resolve().parent.parent.parent
here = Path(__file__).resolve().parent
out = Path(sys.argv[1]).resolve()
app = sys.argv[2]
timeout = int(sys.argv[3]) if len(sys.argv) > 3 else 240
prefix = Path(os.environ.get("TUXOFFICE_PREFIX", ""))
if not str(prefix).endswith("/home/prefix"):
    sys.exit("Use the isolated prefix")
tux = str(root / "tuxoffice")
out.mkdir(parents=True, exist_ok=True)
work = prefix / "drive_c/tuxoffice/functional"
work.mkdir(parents=True, exist_ok=True)
if app != "onenote":
    shutil.copy(here / f"{app}.vbs", work / f"{app}.vbs")
winwork = r"C:\tuxoffice\functional"


def windows():
    try:
        result = sp.run([tux, "wine", "C:/tuxoffice/winctl.exe", "text"], capture_output=True, text=True,
                        errors="replace", timeout=20)
        return result.stdout
    except sp.TimeoutExpired:
        return "winctl timed out\n"


env = dict(os.environ, WINEDEBUG="-all,err+all,warn+seh")
log = (out / f"{app}.log").open("w")
sp.run([tux, "kill"], check=False)
start = time.monotonic()
command = ["C:/tuxoffice/onenote-api.exe", winwork] if app == "onenote" else \
    ["cscript", "//nologo", rf"{winwork}\{app}.vbs", winwork]
process = sp.Popen([tux, "wine", *command],
                   stdout=sp.PIPE, stderr=log, text=True, errors="replace", env=env)
lines = []
seen = ""
(out / f"{app}.txt").write_text("")


def drain():
    for line in process.stdout:
        lines.append(line.rstrip("\r\n"))
        text = f"{time.monotonic() - start:7.1f} {lines[-1]}"
        print(text, flush=True)
        with (out / f"{app}.txt").open("a") as record:
            record.write(text + "\n")


reader = threading.Thread(target=drain, daemon=True)
reader.start()
snapshots = out / f"{app}.windows.txt"
with snapshots.open("w") as snap:
    while process.poll() is None and time.monotonic() - start < timeout:
        time.sleep(5)
        current = windows()
        if current != seen:
            seen = current
            snap.write(f"--- {time.monotonic() - start:.1f}s\n{current}")
            snap.flush()
timed_out = process.poll() is None
if timed_out:
    print("TIMEOUT: windows at the end:", flush=True)
    print("\n".join(line for line in seen.splitlines() if not line.startswith("    ")), flush=True)
    process.kill()
reader.join(5)
sp.run([tux, "kill"], check=False)
log.close()
faults = [line for line in (out / f"{app}.log").read_text(errors="replace").splitlines()
          if "Unhandled exception" in line or "page fault" in line]
summary = {
    "app": app,
    "seconds": round(time.monotonic() - start, 1),
    "timed_out": timed_out,
    "passed": sum(line.startswith("PASS") for line in lines),
    "failed": [line for line in lines if line.startswith("FAIL")],
    "times": [line for line in lines if line.startswith("TIME")],
    "unhandled": faults[:10],
    "completed": any(line.startswith(f"RESULT {app} ") for line in lines),
    "exit_code": process.returncode,
}
(out / f"{app}.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
sys.exit(1 if timed_out or summary["failed"] or faults or not summary["completed"] or process.returncode else 0)
