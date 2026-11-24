#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A failed native chooser helper must open Wine's fallback, never hang the caller.

Run through isolated-run.py SESSION python3 tests/native-dialog-failure.py OUTDIR.
Requires file-dialog-probe.exe and winctl.exe from tests/build-helpers.sh.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
assert os.environ.get("TUXOFFICE_PREFIX", "").endswith("/home/prefix"), "Use an isolated prefix"
out.mkdir(parents=True, exist_ok=True)
results = []
wait_seconds = float(os.environ.get("DIALOG_FAILURE_TIMEOUT", "15"))


def wine(*args):
    # Wine's startup services may inherit output handles. A temporary file
    # lets us wait for the helper process without waiting for pipe EOF too.
    with tempfile.TemporaryFile(mode="w+") as output:
        result = sp.run([str(root / "tuxoffice"), "wine", *args], stdout=output,
                        stderr=output, text=True, timeout=20)
        output.seek(0)
        result.stdout = output.read()
    return result


try:
    sp.run([str(root / "tuxoffice"), "kill"], check=True)
    # Finish runner/prefix startup before a second launcher queries windows.
    sp.run([str(root / "tuxoffice"), "wine", "cmd", "/c", "exit", "0"],
           stdout=sp.DEVNULL, stderr=sp.DEVNULL, timeout=45, check=True)
    for helper in ("/bin/true", "/bin/false", "/nonexistent/tuxoffice-chooser"):
        env = dict(os.environ, TUXOFFICE_FILE_DIALOG=helper, WINEDEBUG="-all,err+all")
        started = time.monotonic()
        with (out / (Path(helper).name + ".log")).open("w") as log:
            process = sp.Popen([str(root / "tuxoffice"), "wine", "C:/tuxoffice/file-dialog-probe.exe", "open"],
                               env=env, stdout=sp.PIPE, stderr=log, text=True, errors="replace")
            deadline = started + wait_seconds
            while True:
                listing = wine("C:/tuxoffice/winctl.exe", "list", "file-dialog-probe.exe").stdout
                if "class=#32770" in listing and "title=Open" in listing:
                    break
                assert time.monotonic() < deadline, f"No fallback after {helper}: {listing}"
                assert process.poll() is None, f"Probe exited before showing fallback: {helper}"
                time.sleep(.1)
            elapsed = time.monotonic() - started
            wine("C:/tuxoffice/winctl.exe", "close", "Open")
            stdout, _ = process.communicate(timeout=10)
            assert process.returncode == 0 and "cancelled" in stdout, stdout
            results.append({"helper": helper, "fallback_seconds_including_startup": round(elapsed, 3),
                            "status": "passed"})
            print(json.dumps(results[-1]), flush=True)
finally:
    sp.run([str(root / "tuxoffice"), "kill"], check=False)
    (out / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
