#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Select files in the real KDE picker and verify 32/64-bit dialog results.

Usage: isolated-run.py SESSION python3 tests/native-dialog-results.py OUTDIR
Requires the two file-dialog-probe executables, KWin and libei. Checks multiple
selection, paths with spaces, save destinations and automatic default extension.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
prefix = Path(os.environ.get("TUXOFFICE_PREFIX", ""))
assert str(prefix).endswith("/home/prefix"), "Use an isolated prefix"
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
work = prefix / "drive_c/tuxoffice/native-dialog-results"
work.mkdir(parents=True, exist_ok=True)
inputs = [work / "first document.docx", work / "second document.docx"]
for path in inputs:
    path.write_text("native chooser fixture\n")
eis = out / "eis-input"
flags = sp.check_output(["pkg-config", "--cflags", "--libs", "libei-1.0", "libsystemd"], text=True).split()
sp.run(["cc", "-O2", "-o", str(eis), str(root / "tests/eis-input.c"), *flags], check=True)
results = []


def windows():
    with tempfile.NamedTemporaryFile(mode="w", suffix=".js") as script:
        script.write("for(const w of workspace.windowList()) if(w.resourceClass==="
                     "'org.freedesktop.impl.portal.desktop.kde') "
                     "print('TUXPICKER',String(w.internalId),w===workspace.activeWindow);")
        script.flush()
        return sp.check_output([str(root / "tests/kwin-run.sh"), script.name], text=True, timeout=15)


try:
    sp.run([str(root / "tuxoffice"), "kill"], check=True)
    for architecture in ("64", "32"):
        probe = "file-dialog-probe32.exe" if architecture == "32" else "file-dialog-probe.exe"
        for mode in ("open", "save"):
            destination = work / ("new document " + architecture)
            destination.with_suffix(".docx").unlink(missing_ok=True)
            text = " ".join('"' + str(path) + '"' for path in inputs) if mode == "open" else str(destination)
            expected = ["C:\\tuxoffice\\native-dialog-results\\" + path.name for path in inputs] if mode == "open" else [
                "C:\\tuxoffice\\native-dialog-results\\" + destination.name + ".docx"]
            with (out / f"{architecture}-{mode}.log").open("w+") as log:
                process = sp.Popen([str(root / "tuxoffice"), "wine", "C:/tuxoffice/" + probe, mode],
                                   stdout=log, stderr=log, env=dict(os.environ,
                                   WINEDEBUG=os.environ.get("DIALOG_RESULTS_WINEDEBUG", "-all")))
                try:
                    deadline = time.monotonic() + 45
                    while "true" not in windows():
                        assert process.poll() is None, "Probe exited before native picker appeared"
                        assert time.monotonic() < deadline, "Native picker did not activate"
                        time.sleep(.2)
                    commands = sp.check_output([sys.executable, str(root / "tests/eis-type.py"), "{ctrl+a}", text, "{enter}"])
                    sp.run([str(eis)], input=commands, check=True, timeout=20, stdout=sp.DEVNULL)
                    process.wait(timeout=20)
                    log.seek(0)
                    output = log.read()
                    actual = [line.removeprefix("result ") for line in output.splitlines() if line.startswith("result ")]
                    assert process.returncode == 0 and sorted(actual) == sorted(expected) and "type 1" in output, output
                    results.append({"architecture": architecture, "mode": mode, "paths": actual, "passed": True})
                    print(json.dumps(results[-1]), flush=True)
                    deadline = time.monotonic() + 10
                    while windows():
                        assert time.monotonic() < deadline, "Previous picker did not close"
                        time.sleep(.1)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
finally:
    sp.run([str(root / "tuxoffice"), "kill"], check=False)
    (out / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
