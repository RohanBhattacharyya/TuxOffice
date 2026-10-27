#!/usr/bin/env python3
"""Compare an actual composition scene with and without a drop shadow.

Usage: shadow-render.py OUTDIR SCENE.exe
Build shadow-scene.c against the current Wine headers first.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import time

from PIL import Image

root=Path(__file__).resolve().parent.parent
out=Path(sys.argv[1]).resolve()
scene=Path(sys.argv[2]).resolve()
if not os.environ.get("TUXOFFICE_PREFIX","").endswith("/home/prefix"):
    sys.exit("Rendering tests require an isolated prefix")
out.mkdir(parents=True,exist_ok=True)
results={}
for mode in ("none","shadow","masked","inherit","surface"):
    stop=out/(mode+".stop")
    stop.unlink(missing_ok=True)
    log_path=out/(mode+".log")
    with log_path.open("w") as log:
        process=sp.Popen([str(root/"tuxoffice"),"wine","Z:"+str(scene).replace("/","\\"),
                          "Z:"+str(stop).replace("/","\\"),mode],stdout=log,stderr=log)
    try:
        activate=out/"activate.js"
        activate.write_text('for(const w of workspace.windowList()){if(w.caption === "TuxOffice shadow regression"){workspace.raiseWindow(w);print("TUXSHADOW WINDOW");break;}}')
        deadline=time.monotonic()+25
        while True:
            assert process.poll() is None, "Scene exited before rendering: "+log_path.read_text()
            shown=sp.run([str(root/"tests/kwin-run.sh"),str(activate)],capture_output=True,text=True,check=True)
            if "TUXSHADOW WINDOW" in shown.stdout and "READY" in log_path.read_text(): break
            if time.monotonic()>deadline: raise RuntimeError("Scene did not become ready")
            time.sleep(.1)
        image_path=out/(mode+".png")
        sp.run([str(root/"tests/capture-word.py"),str(image_path),"--title","TuxOffice shadow regression"],check=True)
        picture=Image.open(image_path).convert("RGB")
        red=[(i%picture.width,i//picture.width) for i,(r,g,b) in enumerate(picture.get_flattened_data()) if r>200 and g<30 and b<30]
        assert len(red)>1500,"The red visual did not render"
        left,right=min(p[0] for p in red),max(p[0] for p in red)
        top,bottom=min(p[1] for p in red),max(p[1] for p in red)
        width,height=right-left+1,bottom-top+1
        x,y=round(right+.15*width),round((top+bottom)/2+.15*height)
        values=[picture.getpixel((x+dx,y+dy)) for dx in range(-2,3) for dy in range(-2,3)]
        mean=sum(sum(p) for p in values)/(3*len(values))
        assert max(max(p)-min(p) for p in values)<8,"Shadow probe should be neutral grey"
        results[mode]={"red_bounds":[left,top,right,bottom],"probe":[x,y],"mean_brightness":round(mean,2)}
        if mode in ("none","masked"): assert mean>245,"Control/masked shadow should have a white background"
        else: assert 10<mean<220,"A visible shadow did not appear outside the visual"
        sp.run([str(root/"tests/check-render-faults.py"),str(log_path)],check=True)
    finally:
        stop.touch()
        try: process.wait(timeout=15)
        except sp.TimeoutExpired:
            sp.run([str(root/"tuxoffice"),"kill"],check=True)
            raise
    assert process.returncode==0,"Scene failed cleanup"
results["status"]="passed"
(out/"summary.json").write_text(json.dumps(results,indent=2)+"\n")
print(json.dumps(results),flush=True)
