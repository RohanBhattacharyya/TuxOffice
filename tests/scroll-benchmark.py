#!/usr/bin/env python3
"""Repeat Office scrolling on the desktop using a disposable Office prefix.

Usage: scroll-benchmark.py OUTDIR DOCUMENT LOGICAL_X LOGICAL_Y [REPEATS] [APP] [TYPE_TEXT]
The capture coordinates must cover document text. Results include capture
overhead; compare runs on the same desktop, document, scale and input cadence.
"""
import json
import os
from pathlib import Path
import statistics
import subprocess as sp
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
document = Path(sys.argv[2]).resolve()
x, y = map(int, sys.argv[3:5])
repeats = int(sys.argv[5]) if len(sys.argv) > 5 else 3
app = sys.argv[6] if len(sys.argv) > 6 else "word"
typing = sys.argv[7] if len(sys.argv) > 7 else None
if typing and app != "word":
    sys.exit("Typing verification currently supports Word only")
if app not in ("word", "excel", "powerpoint"):
    sys.exit("Unknown Office app")
prefix = os.environ.get("TUXOFFICE_PREFIX", "")
if not prefix.endswith("/home/prefix"):
    sys.exit("This benchmark requires an isolated Office prefix")
if not document.is_file() or not 1 <= repeats <= 20:
    sys.exit("Invalid document or repeat count")
out.mkdir(parents=True, exist_ok=True)


def helper(name, *args, timeout=15):
    with tempfile.TemporaryFile(mode="w+") as log:
        result = sp.run([str(root / "tuxoffice"), "wine", f"C:/tuxoffice/{name}.exe", *map(str, args)],
                        stdout=log, stderr=log, timeout=timeout)
        log.seek(0)
        output = log.read()
    with (out / "actions.log").open("a") as log:
        log.write(f"{name} {args} exit={result.returncode}\n{output}")
    return result.returncode, output


try:
    helper("office-probe", app + "-quit", timeout=10)
except sp.TimeoutExpired:
    pass
sp.run([str(root / "tuxoffice"), "kill"], check=True)
trigger = out / "trigger"
env = os.environ.copy()
env["WINEDEBUG"] = env.get("WINEDEBUG", "-all,err+all,warn+seh")
env["TUX_GDB_APP"] = app
launcher = sp.Popen([str(root / "tests/gdb-word.sh"), str(trigger), str(out / "gdb.txt"), str(document)], env=env)
results = []
def median(key):
    values = [r[key] for r in results if r[key] is not None]
    return statistics.median(values) if values else None

try:
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        if app == "word":
            helper("typer", "dismiss-startup")
        status, response = helper("typer", "--app", app, "ping", 700)
        if status == 0 and any(line.startswith("ok ") for line in response.splitlines()):
            break
        time.sleep(.5)
    else:
        raise RuntimeError(f"{app} did not become ready")
    # Do not force SpellingErrors.Count: that would add document-wide proofing
    # work immediately before measuring ordinary scrolling.
    time.sleep(10)
    extra = os.environ.get("TUXOFFICE_BENCH_SECOND_DOCUMENT")
    if extra:
        if app != "word":
            raise RuntimeError("The second-document scenario currently supports Word only")
        if helper("office-probe", "word-open", "Z:" + str(Path(extra).resolve()).replace("/", "\\"), timeout=30)[0]:
            raise RuntimeError("Could not open the second document in the same Word instance")
        time.sleep(3)
    if app == "powerpoint" and os.environ.get("TUXOFFICE_BENCH_EXPECT_SLIDES"):
        status, response = helper("office-probe", "powerpoint-slide-count", timeout=15)
        if status or "slides=" + os.environ["TUXOFFICE_BENCH_EXPECT_SLIDES"] not in response.splitlines():
            raise RuntimeError("PowerPoint did not import the complete slide fixture")
    activate = [sys.executable,str(root / "tests/activate-office.py"),app,document.stem]
    for index in range(repeats):
        sp.run(activate, check=True)
        deadline = time.monotonic() + 5
        while document.stem not in helper("typer", "--app", app, "foreground")[1]:
            if time.monotonic() >= deadline:
                raise RuntimeError("Wine foreground window does not match the selected document")
            time.sleep(.1)
        if helper("typer", "--app", app, "maximize")[0]:
            raise RuntimeError("Could not maximize the selected document")
        if helper("typer", "--app", app, "raise")[0]:
            raise RuntimeError("Could not synchronize Wine's document stacking")
        if helper("typer", "--app", app, "chord", "0x11", "0x24")[0]:
            raise RuntimeError(f"{app} lost focus")
        if helper("typer", "--app", app, "hover", 120 if app == "powerpoint" else 1050, 600)[0]:
            raise RuntimeError("Could not position pointer")
        time.sleep(3)
        if document.stem not in helper("typer", "--app", app, "hit-test")[1]:
            raise RuntimeError("Mouse input would reach a different document")
        if index == 0:
            sp.run([str(root / "tests/capture-word.py"), str(out / "document.png"), app, "--active"], check=True, timeout=15)
        measurement = out / f"round-{index+1}"
        command = [str(root / "tests/scroll-capture.py"), str(measurement), str(x), str(y), app]
        if typing:
            command.extend(["--type", typing])
        sp.run(command, check=True)
        results.append(json.loads((measurement / "summary.json").read_text()))
        if typing:
            status, text = helper("office-probe", "word-text", timeout=30)
            if status or typing not in text:
                raise RuntimeError("Word did not retain the injected text")
    sp.run([str(root / "tests/check-render-faults.py"), str(out / "gdb.txt.wine.log")], check=True)
    assert "Unhandled" not in (out / "gdb.txt.wine.log").read_text(errors="replace"), "Office crashed during scrolling"
    summary = {"status":"passed" if all(r["visible_changes_during_input"] for r in results) else "no-visible-updates",
               "app":app, "action":"typing" if typing else "scrolling", "document":str(document), "runner":os.environ.get("TUXOFFICE_RUNNER"),
               "wine_debug":env["WINEDEBUG"],
               "rounds":results, "median":{key:median(key)
               for key in ("first_visible_change_ms", "p95_change_interval_ms", "longest_change_interval_ms", "visible_changes_per_second", "app_process_cpu_seconds")}}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
finally:
    trigger.touch()
    try:
        launcher.wait(timeout=25)
    except sp.TimeoutExpired:
        launcher.terminate()
    try:
        helper("office-probe", app + "-quit", timeout=10)
    finally:
        sp.run([str(root / "tuxoffice"), "kill"], check=True)
sys.exit(0 if results and all(r["visible_changes_during_input"] for r in results) else 1)
