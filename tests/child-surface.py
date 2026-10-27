#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Capture GPU children across layout/present delays, without Office.

Usage: child-surface.py OUTDIR SCENE.exe [transparent]
Use a disposable prefix whose path contains child-surface.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time

from PIL import Image, ImageChops
from office_windows import prefix_pids
from screen_area import WindowCapture

root=Path(__file__).resolve().parent.parent
out=Path(sys.argv[1]).resolve()
scene=Path(sys.argv[2]).resolve()
assert 'child-surface' in str(Path(os.environ['TUXOFFICE_PREFIX']).resolve())
out.mkdir(parents=True,exist_ok=True)
stop=out/'stop';stop.unlink(missing_ok=True)
sp.run([str(root/'tuxoffice'),'kill'],check=True)
summary={'status':'failed'}
client=None
try:
    with (out/'scene.log').open('w') as log:
        client=sp.Popen([str(root/'tuxoffice'),'wine','Z:'+str(scene).replace('/','\\'),
                         'Z:'+str(stop).replace('/','\\'),*sys.argv[3:]],stdout=log,stderr=log)
    deadline=time.monotonic()+40
    while 'READY child surfaces' not in (out/'scene.log').read_text(errors='replace'):
        assert client.poll() is None,'Scene exited before presenting'
        assert time.monotonic()<deadline,'Scene did not become ready'
        time.sleep(.1)
    with tempfile.NamedTemporaryFile(mode='w',suffix='.js') as script:
        script.write('const pids='+json.dumps(prefix_pids())+';for(const w of workspace.windowList()){if(pids.includes(w.pid)&&w.caption==="Child GPU surface regression")print("TUXSCENE",w.internalId);}')
        script.flush()
        text=sp.check_output([str(root/'tests/kwin-run.sh'),script.name],text=True)
    handle=next(line.split(' ',1)[1] for line in text.splitlines() if line.startswith('TUXSCENE '))
    capture=WindowCapture(handle)
    boxes=[]
    for attempt in range(40):
        info,data=capture.capture();picture=capture.image(info,data).convert('RGB')
        channels=picture.split();boxes=[]
        for color in range(3):
            mask=channels[color].point(lambda v:255 if v>150 else 0)
            for other in range(3):
                if color!=other:mask=ImageChops.multiply(mask,channels[other].point(lambda v:255 if v<50 else 0))
            mask.paste(0,(0,0,picture.width,50))
            mask.paste(0,(0,0,10,picture.height))
            mask.paste(0,(picture.width-10,0,picture.width,picture.height))
            box=mask.getbbox()
            if not box or min(box[2]-box[0],box[3]-box[1])<60:break
            x,y,r,b=box
            boxes.append((round(x+(r-x)*.25),round(y+(b-y)*.25),round(r-(r-x)*.25),round(b-(b-y)*.25)))
        if len(boxes)==3:break
        time.sleep(.02)
    picture.save(out/'before.png')
    assert len(boxes)==3,'Initial GPU panels did not render'
    began=time.monotonic();frames=flashes=0
    while time.monotonic()-began<8:
        info,data=capture.capture();picture=capture.image(info,data).convert('RGB')
        valid=True
        for color,box in enumerate(boxes):
            pixels=picture.crop(box).resize((12,12),Image.Resampling.NEAREST).get_flattened_data()
            matching=sum(p[color]>150 and all(p[j]<50 for j in range(3) if j!=color) for p in pixels)
            valid=valid and matching>=140
        if not valid:
            if flashes<5:picture.save(out/f'flash-{frames}.png')
            flashes+=1
        frames+=1
        time.sleep(.004)
    summary.update(status='passed' if not flashes else 'flicker-detected',frames=frames,flash_frames=flashes)
except Exception as error:
    summary['error']=str(error)
finally:
    stop.touch()
    if client:
        try:summary['process_exit']=client.wait(timeout=20)
        except sp.TimeoutExpired:summary['status']='failed'
    sp.run([str(root/'tuxoffice'),'kill'],check=True)
    if summary.get('process_exit')!=0:summary['status']='failed'
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary),flush=True)
sys.exit(0 if summary['status']=='passed' else 1)
