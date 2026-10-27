#!/usr/bin/env python3
"""Measure spelling-card visibility and completed pixels in a test prefix.

Usage: popup-latency.py OUTDIR [DOCUMENT] [REPEATS]
Completed pixels are identified retrospectively against the final card image.
The first screenshot matching that image is an upper bound on rendering time.
"""
import json
import os
from pathlib import Path
import re
import statistics
import subprocess as sp
import sys
import tempfile
import time
import zlib

from screen_area import ScreenArea
from office_windows import window_script

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
document = Path(sys.argv[2]).resolve() if len(sys.argv)>2 else Path.home()/"Documents/TuxOffice-test/render-test.docx"
repeats = int(sys.argv[3]) if len(sys.argv)>3 else 5
prefix = os.environ.get("TUXOFFICE_PREFIX", "")
if not prefix.endswith("/home/prefix"):
    sys.exit("This test requires an isolated Office prefix")
os.umask(0o077)
out.mkdir(parents=True, exist_ok=True)
clock = lambda: time.clock_gettime(time.CLOCK_MONOTONIC_RAW)


def helper(name, *args, timeout=20):
    with tempfile.TemporaryFile(mode="w+") as log:
        status = sp.run([str(root/"tuxoffice"), "wine", f"C:/tuxoffice/{name}.exe", *map(str,args)],
                        stdout=log, stderr=log, timeout=timeout).returncode
        log.seek(0)
        text = log.read()
    with (out/"actions.log").open("a") as log:
        log.write(f"{name} {args} exit={status}\n{text}")
    return status,text


sp.run([str(root/"tuxoffice"), "kill"],check=True)
trigger = out/"trigger"
env = os.environ.copy()
env["WINEDEBUG"] = "-all,err+all,warn+seh"
launcher = sp.Popen([str(root/"tests/gdb-word.sh"), str(trigger), str(out/"gdb.txt"), str(document)],env=env)
results = []
try:
    deadline = clock()+90
    while clock()<deadline:
        helper("typer","dismiss-startup")
        if helper("typer","ping",700)[1].startswith("ok"):
            break
        time.sleep(.5)
    else:
        raise RuntimeError("Word did not become ready")
    time.sleep(8)
    status,text = helper("office-probe","word-click-point")
    coordinates = re.findall(r"^(\d+) (\d+)\s*$",text,re.M)
    if status or not coordinates:
        raise RuntimeError("Could not locate the test misspelling")
    x,y = map(int,coordinates[-1])
    sp.run([sys.executable,str(root/"tests/activate-office.py"),"word",document.stem],check=True)
    assert helper("typer","raise")[0]==0, "Could not synchronize document stacking"
    assert document.stem in helper("typer","foreground")[1], "Wrong document has keyboard focus"
    helper("typer","hover",x,y)
    assert document.stem in helper("typer","hit-test")[1], "Mouse input would reach another document"
    # Wayland can place the native toplevel somewhere other than Wine's
    # virtual desktop origin. Anchor the popup capture to its actual parent.
    with tempfile.NamedTemporaryFile(mode="w",suffix=".js") as script:
        script.write(window_script("word",document.stem,geometry=True));script.flush()
        text=sp.check_output([str(root/"tests/kwin-run.sh"),script.name],text=True)
    geometry=json.loads(next(line.split(" ",1)[1] for line in text.splitlines() if line.startswith("TUXGEOMETRY ")))
    status,text=helper("typer","window-rect")
    assert status==0, "Could not read Word's virtual window rectangle"
    parent_rect=tuple(map(int,text.split()))
    dx,dy=round(geometry["x"])-parent_rect[0],round(geometry["y"])-parent_rect[1]
    area=ScreenArea(max(0,x+dx-70),max(0,y+dy-10),420,320)
    for index in range(repeats):
        helper("typer","key","0x1b")
        time.sleep(.6)
        samples=[]
        log_path=out/f"round-{index+1}-input.txt"
        with log_path.open("w") as log:
            process=sp.Popen([str(root/"tuxoffice"),"wine","C:/tuxoffice/typer.exe","popup-bench",str(x),str(y),"1"],stdout=log,stderr=log)
            deadline=clock()+7
            event=None
            while clock()<deadline:
                info,data=area.capture()
                samples.append((clock()*1000,info,data))
                if not event:
                    event=re.search(r"click=(\d+) visible=(\d+) delay=(\d+).*rect=([\d,-]+)",log_path.read_text(errors="replace"))
                    if event:
                        deadline=int(event[1])/1000+2
                time.sleep(.02)
            assert process.wait(timeout=10)==0 and event, "Card input failed or lost focus"
        status,text=helper("typer","popup-rect")
        assert status==0, "Spelling card disappeared"
        rect=tuple(map(int,text.strip().split()))
        scale_x=int(samples[-1][1]["width"])/area.width
        scale_y=int(samples[-1][1]["height"])/area.height
        box=(round((rect[0]+dx-area.x)*scale_x),round((rect[1]+dy-area.y)*scale_y),
             round((rect[2]+dx-area.x)*scale_x),round((rect[3]+dy-area.y)*scale_y))
        assert 0<=box[0]<box[2]<=int(samples[-1][1]["width"]) and 0<=box[1]<box[3]<=int(samples[-1][1]["height"]), "Card extends outside capture area"
        crops=[(tick,area.image(info,data).convert("RGB").crop(box)) for tick,info,data in samples if tick>=int(event[1])]
        final=crops[-1][1]
        final.save(out/f"round-{index+1}-final.png")
        crops[0][1].save(out/f"round-{index+1}-first.png")
        assert sum(max(pixel)<160 for pixel in final.get_flattened_data())>200, "Final card contains too little text/icon content"
        toolbar=[]
        for left,right in ((.05,.20),(.22,.37),(.40,.55)):
            icon=final.crop((round(left*final.width),final.height-40,round(right*final.width),final.height-8))
            toolbar.append(sum(max(pixel)<160 for pixel in icon.get_flattened_data()))
        assert all(count>=3 for count in toolbar), "One or more spelling-card action icons are missing"
        expected=zlib.crc32(final.tobytes())
        matches=[tick for tick,image in crops if zlib.crc32(image.tobytes())==expected]
        result={"round":index+1,"visible_window_ms":int(event[3]),
                "completed_pixels_upper_bound_ms":round(matches[0]-int(event[1]),2),
                "samples":len(crops),"rect_logical":rect,"toolbar_dark_pixels":toolbar}
        results.append(result)
        print(json.dumps(result),flush=True)
    sp.run([str(root/"tests/check-render-faults.py"),str(out/"gdb.txt.wine.log")],check=True)
    summary={"status":"passed","clock":"CLOCK_MONOTONIC_RAW", "rounds":results,
             "wine_to_desktop_offset_logical":[dx,dy],
             "median_completed_pixels_upper_bound_ms":statistics.median(r["completed_pixels_upper_bound_ms"] for r in results),
             "measurement":"First capture matching final card pixels; includes capture overhead and requires inspection of saved final images."}
    (out/"summary.json").write_text(json.dumps(summary,indent=2)+"\n")
finally:
    trigger.touch()
    try:
        launcher.wait(timeout=25)
    except sp.TimeoutExpired:
        launcher.terminate()
    try:
        helper("office-probe","word-quit",timeout=10)
    finally:
        sp.run([str(root/"tuxoffice"),"kill"],check=True)
