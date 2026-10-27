#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Measure visible scrolling changes in a small KWin capture area.

Usage: scroll-capture.py OUTDIR LOGICAL_X LOGICAL_Y [word|excel|powerpoint]
The selected app must already be ready and focused in an isolated prefix.
Captures hash pixels in memory; only the initial and final test areas are saved.
"""
import json
import os
from pathlib import Path
import statistics
import subprocess as sp
import sys
import time
import zlib

from screen_area import ScreenArea

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
x, y = map(int, sys.argv[2:4])
app = sys.argv[4] if len(sys.argv) > 4 else "word"
typing = sys.argv[6] if len(sys.argv) > 6 and sys.argv[5] == "--type" else None
interval = int(os.environ.get("TUXOFFICE_BENCH_INTERVAL_MS", "50"))
expected_inputs = len(typing.encode("utf-16le"))//2 if typing else int(os.environ.get("TUXOFFICE_BENCH_INPUTS", "120"))
if not 1 <= interval <= 1000 or expected_inputs < 2 or expected_inputs * interval > 6500:
    sys.exit("Input sequence must fit inside the eight-second capture")
# Wine's monotonic_counter() prefers CLOCK_MONOTONIC_RAW on Linux. CLOCK_MONOTONIC
# can differ by hundreds of milliseconds after clock-rate adjustments; mixing
# the two makes promptly rendered input appear to have a long initial delay.
clock_id = time.CLOCK_MONOTONIC_RAW
def clock():
    return time.clock_gettime(clock_id)

prefix = os.environ.get("TUXOFFICE_PREFIX", "")
if not prefix.endswith("/home/prefix"):
    sys.exit("Scrolling tests require an isolated Office prefix")
if app not in ("word", "excel", "powerpoint"):
    sys.exit("Unknown Office app")
width, height = (1000, 320) if typing else (320, 180)
area = ScreenArea(x,y,width,height)
capture = area.capture


def save_capture(name, info, data):
    area.image(info,data).save(out / name)


def cpu_ticks():
    total = 0
    exe = {"word":"WINWORD.EXE", "excel":"EXCEL.EXE", "powerpoint":"POWERPNT.EXE"}[app]
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            command = (process / "cmdline").read_bytes()
            if not command.startswith(b"C:") or exe.encode() not in command.upper():
                continue
            env = (process / "environ").read_bytes().split(b"\0")
            if ("WINEPREFIX=" + prefix).encode() not in env:
                continue
            values = (process / "stat").read_text().rsplit(")", 1)[1].split()
            total += int(values[11]) + int(values[12])
        except (OSError, ValueError):
            pass
    return total


info, data = capture()
save_capture("area-before.png", info, data)
previous = zlib.crc32(data)
samples = []
started = clock()
cpu_before = cpu_ticks()
with (out / "input.txt").open("w+") as log:
    action = ["type-bench",typing,str(interval)] if typing else ["scroll-bench",str(expected_inputs),str(interval)]
    process = sp.Popen([str(root / "tuxoffice"), "wine", "C:/tuxoffice/typer.exe", "--app", app,
                        *action], stdout=log, stderr=log)
    while clock() < started + 8:
        info, data = capture()
        now = clock()
        digest = zlib.crc32(data)
        samples.append({"time_ms":round(now*1000,3), "changed":digest != previous, "crc":digest})
        previous = digest
        time.sleep(.008)
    status = process.wait(timeout=10)
    log.seek(0)
    import re
    ticks = [int(t) for t in re.findall(r"(?:scroll round|type index)=\d+ tick=(\d+)", log.read())]
cpu_after = cpu_ticks()
save_capture("area-after.png", info, data)
if status or len(ticks) != expected_inputs:
    sys.exit(f"Scrolling helper failed or lost focus: status={status}, inputs={len(ticks)}")
if abs(ticks[0] - started*1000) > 2000:
    sys.exit("Wine input clock does not match the capture clock")
changes = [sample["time_ms"] for sample in samples if sample["changed"] and ticks[0] <= sample["time_ms"] <= ticks[-1]+100]
gaps = [b-a for a,b in zip(changes,changes[1:])]
sorted_gaps = sorted(gaps)
report = {
    "app":app, "action":"typing" if typing else "scrolling", "input_events":len(ticks), "requested_input_interval_ms":interval, "input_duration_ms":ticks[-1]-ticks[0],
    "clock":"CLOCK_MONOTONIC_RAW (same source as Wine GetTickCount64)",
    "capture_samples":len(samples), "visible_changes_during_input":len(changes),
    "visible_changes_per_second":round(len(changes)/((ticks[-1]-ticks[0])/1000),2),
    "first_visible_change_ms":round(changes[0]-ticks[0],2) if changes else None,
    "median_change_interval_ms":round(statistics.median(gaps),2) if gaps else None,
    "p95_change_interval_ms":round(sorted_gaps[min(len(gaps)-1,int(len(gaps)*.95))],2) if gaps else None,
    "longest_change_interval_ms":round(max(gaps),2) if gaps else None,
    "app_process_cpu_seconds":round((cpu_after-cpu_before)/os.sysconf("SC_CLK_TCK"),3),
    "capture_area_logical":[x,y,width,height],
    "measurement":"Pixel changes in one screen area; includes capture overhead and does not establish Windows parity.",
}
(out / "samples.json").write_text(json.dumps(samples) + "\n")
(out / "summary.json").write_text(json.dumps(report,indent=2) + "\n")
print(json.dumps(report),flush=True)
