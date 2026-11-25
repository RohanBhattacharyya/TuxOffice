#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Cancel the real native chooser and check keyboard focus returns to an edit.

Usage: isolated-run.py SESSION python3 tests/native-dialog-focus.py OUTDIR
Requires native-dialog-focus.exe from build-helpers.sh, KWin and libei.
Set DIALOG_FOCUS_ROUNDS to repeat up to 100 times.
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
eis = out / "eis-input"
flags = sp.check_output(["pkg-config", "--cflags", "--libs", "libei-1.0", "libsystemd"], text=True).split()
sp.run(["cc", "-O2", "-o", str(eis), str(root / "tests/eis-input.c"), *flags], check=True)
results = []
rounds = int(os.environ.get("DIALOG_FOCUS_ROUNDS", "3"))
assert 1 <= rounds <= 100
try:
    sp.run([str(root / "tuxoffice"), "kill"], check=True)
    for attempt in range(rounds):
        with (out / f"focus-{attempt}.log").open("w+") as log:
            process = sp.Popen([str(root / "tuxoffice"), "wine", "C:/tuxoffice/native-dialog-focus.exe"],
                               stdout=log, stderr=log,
                               env=dict(os.environ, WINEDEBUG=os.environ.get("DIALOG_FOCUS_WINEDEBUG", "-all")))
            deadline = time.monotonic() + 45
            try:
                with tempfile.NamedTemporaryFile(mode="w", suffix=".js") as script:
                    script.write("for(const w of workspace.windowList()) if(w.resourceClass==="
                                 "'org.freedesktop.impl.portal.desktop.kde' && w===workspace.activeWindow) "
                                 "print('TUXCHOOSER ACTIVE');")
                    script.flush()
                    while True:
                        listing = sp.check_output([str(root / "tests/kwin-run.sh"), script.name],
                                                  text=True, timeout=15)
                        if "TUXCHOOSER ACTIVE" in listing:
                            break
                        assert process.poll() is None, "Probe exited before showing the chooser"
                        assert time.monotonic() < deadline, "Native chooser did not activate"
                        time.sleep(.2)
                commands = sp.check_output([sys.executable, str(root / "tests/eis-type.py"), "{escape}"])
                sp.run([str(eis)], input=commands, check=True, timeout=10, stdout=sp.DEVNULL)
                process.wait(timeout=15)
                log.seek(0)
                output = log.read()
                passed = process.returncode == 0 and "restored_edit_focus=1" in output
                results.append({"attempt": attempt + 1, "passed": passed, "output": output.strip()})
                print(json.dumps(results[-1]), flush=True)
                assert passed, output
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
finally:
    sp.run([str(root / "tuxoffice"), "kill"], check=False)
    (out / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
