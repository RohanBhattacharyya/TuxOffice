#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Capture stable document pixels during cell entry and ribbon changes.

Usage: office-flicker.py OUTDIR word|excel|powerpoint ribbon|cells [PRESENTATION.pptx]
Fixtures are disposable. Saturated document markers must remain visible,
and the ribbon must retain content during each transition.
For PowerPoint, supply the presentation produced by office-suite.py.
"""
import json
import os
from collections import Counter
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time
from zipfile import ZipFile, ZIP_DEFLATED

from PIL import Image
from office_windows import window_script
from screen_area import WindowCapture

root=Path(__file__).resolve().parent.parent
os.umask(0o077)
out=Path(sys.argv[1]).resolve()
app,action=sys.argv[2:4]
assert os.environ.get('TUXOFFICE_PREFIX','').endswith('/home/prefix'), 'Use the disposable prefix'
assert app in ('word','excel','powerpoint') and action in ('ribbon','cells','collapse')
assert action!='cells' or app=='excel'
assert action!='collapse' or app=='word'
out.mkdir(parents=True,exist_ok=True)
fixture=out/('flicker-'+app+('.docx' if app=='word' else '.xlsx'))
types='<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>'
rels='<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
if app=='word':
    rows=[]
    for y in range(4):
        cells=''.join('<w:tc><w:tcPr><w:tcW w:w="1500" w:type="dxa"/><w:shd w:fill="'+('174F2B' if (x+y)%2 else '8CDD7B')+'"/></w:tcPr><w:p/></w:tc>' for x in range(4))
        rows.append('<w:tr><w:trPr><w:trHeight w:val="600" w:hRule="exact"/></w:trPr>'+cells+'</w:tr>')
    files={'[Content_Types].xml':types+'<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>',
           '_rels/.rels':rels+'<Relationship Id="r1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>',
           'word/document.xml':'<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Unchanging document marker</w:t></w:r></w:p><w:tbl><w:tblPr><w:tblW w:w="6000" w:type="dxa"/></w:tblPr><w:tblGrid>'+('<w:gridCol w:w="1500"/>'*4)+'</w:tblGrid>'+''.join(rows)+'</w:tbl><w:sectPr><w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" w:bottom="1440" w:left="1440" w:right="1440"/></w:sectPr></w:body></w:document>'}
elif app=='excel':
    rows=[]
    for y in range(2,12):
        cells=''.join(f'<c r="{chr(77+x)}{y}" s="{1+(x+y)%2}"/>' for x in range(10))
        rows.append(f'<row r="{y}">{cells}</row>')
    files={'[Content_Types].xml':types+'<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/></Types>',
           '_rels/.rels':rels+'<Relationship Id="r1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>',
           'xl/workbook.xml':'<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Entry" sheetId="1" r:id="r1"/></sheets></workbook>',
           'xl/_rels/workbook.xml.rels':rels+'<Relationship Id="r1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="r2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>',
           'xl/worksheets/sheet1.xml':'<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetViews><sheetView workbookViewId="0"/></sheetViews><sheetData>'+''.join(rows)+'</sheetData></worksheet>',
           'xl/styles.xml':'<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><fonts count="1"><font><sz val="11"/><name val="Calibri"/></font></fonts><fills count="4"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill><fill><patternFill patternType="solid"><fgColor rgb="FF174F2B"/></patternFill></fill><fill><patternFill patternType="solid"><fgColor rgb="FF8CDD7B"/></patternFill></fill></fills><borders count="1"><border/></borders><cellStyleXfs count="1"><xf/></cellStyleXfs><cellXfs count="3"><xf fontId="0" fillId="0" borderId="0" xfId="0"/><xf fontId="0" fillId="2" borderId="0" xfId="0" applyFill="1"/><xf fontId="0" fillId="3" borderId="0" xfId="0" applyFill="1"/></cellXfs></styleSheet>'}
else:
    assert len(sys.argv)>4,'Supply the fresh presentation produced by office-suite.py'
    fixture=Path(sys.argv[4]).resolve()
    assert fixture.is_file(),'PowerPoint fixture is missing'
    files={}
if files:
    with ZipFile(fixture,'w',ZIP_DEFLATED) as archive:
        for name,content in files.items():archive.writestr(name,content)

ribbon_tabs={'word':('Home','Insert','Layout'),'excel':('Home','Insert','Page Layout'),
             'powerpoint':('Home','Insert','Design')}

def helper(*args,timeout=15):
    with tempfile.TemporaryFile(mode='w+') as output:
        p=sp.run([str(root/'tuxoffice'),'wine','C:/tuxoffice/typer.exe','--app',app,*map(str,args)],stdout=output,stderr=output,timeout=timeout)
        output.seek(0);text=output.read()
    with (out/'actions.log').open('a') as log:log.write(str(args)+' exit='+str(p.returncode)+'\n'+text)
    return p.returncode,text

trigger=out/'trigger'
launcher=None
summary={'status':'failed','app':app,'action':action,'rounds':[]}
try:
    sp.run([str(root/'tuxoffice'),'kill'],check=True)
    # Office remembers a collapsed ribbon (a toggle lost in an earlier collapse run
    # leaves it so); every run starts from the default, expanded ribbon.
    for name in ('Word','Excel','PowerPoint'):
        sp.run([str(root/'tuxoffice'),'wine','reg','delete',
                rf'HKCU\Software\Microsoft\Office\16.0\Common\Toolbars\{name}','/v','QuickAccessToolbarStyle','/f'],
               stdout=sp.DEVNULL,stderr=sp.DEVNULL)
    sp.run([str(root/'tuxoffice'),'kill'],check=True)
    env=dict(os.environ,TUX_GDB_APP=app,WINEDEBUG=os.environ.get('WINEDEBUG','-all,err+all,warn+seh'))
    launcher=sp.Popen([str(root/'tests/gdb-word.sh'),str(trigger),str(out/'gdb.txt'),str(fixture)],env=env)
    deadline=time.monotonic()+90
    while True:
        if app=='word':helper('dismiss-startup')
        if any(line.startswith('ok ') for line in helper('ping',700)[1].splitlines()):break
        if time.monotonic()>deadline:raise TimeoutError('App did not become ready')
        time.sleep(.3)
    time.sleep(8)
    if os.environ.get('TUXOFFICE_GDB_EARLY'):
        trigger.touch()
        deadline=time.monotonic()+10
        while not (out/'gdb.txt').is_file() or 'TUX DEBUG ATTACHED' not in (out/'gdb.txt').read_text(errors='replace'):
            assert time.monotonic()<deadline,'Debugger did not attach'
            time.sleep(.1)
    sp.run([sys.executable,str(root/'tests/activate-office.py'),app,fixture.stem],check=True)
    assert helper('maximize')[0]==0 and helper('raise')[0]==0
    time.sleep(2)
    sp.run([sys.executable,str(root/'tests/activate-office.py'),app,fixture.stem],check=True)
    assert helper('raise')[0]==0
    snapshot=out/'before.png'
    sp.run([str(root/'tests/capture-word.py'),str(snapshot),app,'--active'],check=True)
    picture=Image.open(snapshot).convert('RGB')
    mask=picture.convert('HSV').getchannel('S').point(lambda v:255 if v>70 else 0)
    mask.paste(0,(0,0,picture.width,250))
    mask.paste(0,(0,0,picture.width//4,picture.height))
    mask.paste(0,(picture.width-32,0,picture.width,picture.height))
    mask.paste(0,(0,picture.height-32,picture.width,picture.height))
    candidates=[y for y in range(250,picture.height) if mask.crop((0,y,picture.width,y+1)).histogram()[255]>=120]
    assert candidates,'No stable colored document marker'
    runs=[]
    for y in candidates:
        if not runs or y!=runs[-1][-1]+1:runs.append([])
        runs[-1].append(y)
    rows=max(runs,key=len)
    assert len(rows)>30,'Document marker is too small'
    box=mask.crop((0,rows[len(rows)//2],picture.width,rows[len(rows)//2]+1)).getbbox()
    left,right=box[0],box[2]
    top,bottom=rows[0],rows[-1]+1
    with tempfile.NamedTemporaryFile(mode='w',suffix='.js') as source:
        source.write(window_script(app,fixture.stem,geometry=True));source.flush()
        text=sp.check_output([str(root/'tests/kwin-run.sh'),source.name],text=True)
    geometry=json.loads(next(line.split(' ',1)[1] for line in text.splitlines() if line.startswith('TUXGEOMETRY ')))
    scale=picture.width/geometry['width']
    handle=next(line.split(' ',1)[1] for line in text.splitlines() if line.startswith('TUXCAPTURE '))
    capture=WindowCapture(handle)
    marker_box=(round(left+(right-left)*.2),round(top+(bottom-top)*.2),round(right-(right-left)*.2),round(bottom-(bottom-top)*.2))
    # A collapsing ribbon moves the page by the ribbon's height; track the marker that far.
    reach=260 if action=='collapse' else 20
    tracking_box=(max(0,left-20),max(0,top-reach),min(picture.width,right+20),min(picture.height,bottom+20))
    ribbon_box=tuple(round(v*scale) for v in (20,90,920,175))
    def saturated(image):
        pixels=image.convert('RGB').resize((24,16),Image.Resampling.NEAREST).get_flattened_data()
        return sum(max(p)-min(p)>50 for p in pixels)/384
    initial=picture.crop(marker_box)
    initial.save(out/'marker.png')
    expected=saturated(initial)
    summary['marker']={'window_pixel_rect':marker_box, 'saturated_fraction':expected,
                       'capture_method':'KWin CaptureWindow, independent of desktop focus'}
    assert expected>.85,f'Marker crop is not solid enough: {expected}'
    for trial in range(1,int(os.environ.get('TUXOFFICE_FLICKER_ROUNDS','3'))+1):
        sp.run([sys.executable,str(root/'tests/activate-office.py'),app,fixture.stem],check=True)
        assert helper('raise')[0]==0
        if action=='cells':assert helper('chord','0x11','0x24')[0]==0
        time.sleep(.5)
        path=out/f'input-{trial}.log'
        count=40 if action=='cells' else int(os.environ.get('TUXOFFICE_RIBBON_CHANGES','30'))
        if action=='collapse':count=int(os.environ.get('TUXOFFICE_RIBBON_CHANGES','12'))
        clicks=action=='ribbon' and os.environ.get('TUXOFFICE_RIBBON_INPUT','click')!='keys'
        # Tab clicks closer together than the double-click time collapse the ribbon.
        delay=100 if action=='cells' else 900 if action=='collapse' else int(os.environ.get('TUXOFFICE_INPUT_DELAY','650' if clicks else '140'))
        with path.open('w') as log:
            if action=='cells':command=['cell-bench',str(count),str(delay)]
            elif action=='collapse':command=['chord-bench',str(count),str(delay),'0x11','0x70']
            elif not clicks:command=['ribbon-bench',str(count),str(delay)]
            # Click tabs like a user: a KeyTip sequence that misses Alt types into the document.
            else:command=['ribbon-click-bench',str(count),str(delay),*ribbon_tabs[app]]
            process=sp.Popen([str(root/'tuxoffice'),'wine','C:/tuxoffice/typer.exe','--app',app,*command],stdout=log,stderr=log,env=env)
            frames=bad_document=bad_ribbon=oversized=0
            samples=[]
            ribbon_appearances=set()
            positions=Counter()
            began=time.monotonic()
            while time.monotonic()-began<max(8,count*(delay+60)/1000+2):
                info,data=capture.capture();frame=capture.image(info,data)
                if frame.size!=picture.size:
                    # Collapsing the ribbon must not change the window either, but count it
                    # so transient oversize frames from misplaced subsurfaces are measured.
                    if action!='collapse' or frame.width<picture.width or frame.height<picture.height:
                        frame.save(out/f'resized-{trial}-{frames}.png')
                        raise AssertionError(f'The document window changed size during measurement: {picture.size} -> {frame.size}')
                    if oversized<5:frame.save(out/f'oversized-{trial}-{frames}.png')
                    oversized+=1
                    frame=frame.crop((0,0)+picture.size)
                im=frame.crop(marker_box);rim=frame.crop(ribbon_box)
                ribbon_appearances.add(rim.convert('L').resize((90,10)).tobytes())
                density=sum(rim.convert('L').histogram()[:180])/(rim.width*rim.height)
                doc_bad=action!='collapse' and saturated(im)<expected*.85
                ribbon_bad=density<.004
                bounds=frame.crop(tracking_box).convert('HSV').getchannel('S').point(lambda v:255 if v>70 else 0).getbbox()
                position=None
                if bounds and not doc_bad:
                    position=(tracking_box[0]+bounds[0]-left,tracking_box[1]+bounds[1]-top,
                              bounds[2]-bounds[0],bounds[3]-bounds[1])
                    if position not in positions and len(positions)<5:
                        frame.save(out/f'marker-position-{trial}-{len(positions)}.png')
                    positions[position]+=1
                samples.append({'raw_time_ms':round(time.clock_gettime(time.CLOCK_MONOTONIC_RAW)*1000,3),
                                'document_bad':doc_bad,'ribbon_bad':ribbon_bad,'ribbon_ink':round(density,4),
                                'marker_position':position})
                if doc_bad and bad_document<5:im.save(out/f'document-flash-{trial}-{frames}.png')
                if ribbon_bad and bad_ribbon<5:rim.save(out/f'ribbon-flash-{trial}-{frames}.png')
                bad_document+=doc_bad;bad_ribbon+=ribbon_bad;frames+=1
                time.sleep(.008)
            assert process.wait(timeout=10)==0,'Input failed or lost focus'
            if action=='ribbon' and len(ribbon_appearances)<3:frame.save(out/f'unchanged-ribbon-{trial}.png')
        (out/f'samples-{trial}.json').write_text(json.dumps(samples)+'\n')
        row={'trial':trial,'frames':frames,'document_flash_frames':bad_document,'ribbon_blank_frames':bad_ribbon,
             'ribbon_appearances':len(ribbon_appearances),
             'oversized_frames':oversized,
             'marker_positions':[{'dx':p[0],'dy':p[1],'width':p[2],'height':p[3],'frames':n} for p,n in positions.items()]}
        summary['rounds'].append(row);print(json.dumps(row),flush=True)
        assert 'Unhandled' not in (out/'gdb.txt.wine.log').read_text(errors='replace'),'Office crashed during input'
        assert action!='ribbon' or len(ribbon_appearances)>=3,'The ribbon stopped updating during tab changes'
        assert any(line.startswith('ok ') for line in helper('ping',700)[1].splitlines()),'App stopped responding'
    if action=='cells':
        assert helper('chord','0x11','0x53')[0]==0
        time.sleep(2)
        import xml.etree.ElementTree as ET
        with ZipFile(fixture) as archive:
            sheet=ET.fromstring(archive.read('xl/worksheets/sheet1.xml'))
            actual={c.attrib['r']:c.find('{*}v').text for c in sheet.findall('.//{*}c') if c.find('{*}v') is not None}
        assert all(actual.get('A'+str(i))==str(i) for i in range(1,41)),'Entered cells did not survive saving'
        assert helper('key','0x5d')[0]==0
        time.sleep(1)
        sp.run([str(root/'tests/capture-word.py'),str(out/'context-menu.png'),app],check=True)
        helper('key','0x1b')
    sp.run([sys.executable,str(root/'tests/check-floating-strips.py')],check=True)
    sp.run([str(root/'tests/check-render-faults.py'),str(out/'gdb.txt.wine.log')],check=True)
    assert 'Unhandled' not in (out/'gdb.txt.wine.log').read_text(errors='replace'), 'Office crashed; see the original exception in gdb.txt.wine.log'
    summary['status']='passed' if all(not r['document_flash_frames'] and not r['ribbon_blank_frames'] for r in summary['rounds']) else 'flicker-detected'
except Exception as error:
    summary['error']=str(error)
    with tempfile.NamedTemporaryFile(mode='w',suffix='.js') as source:
        source.write('const w=workspace.activeWindow;if(w)print("TUXACTIVE",JSON.stringify({pid:w.pid,app:w.resourceClass,officeCaption:["winword.exe","excel.exe","powerpnt.exe"].includes(w.resourceClass)?w.caption:""}));')
        source.flush()
        summary['active_window_at_failure']=sp.check_output([str(root/'tests/kwin-run.sh'),source.name],text=True).strip()
finally:
    trigger.touch()
    if launcher:
        try:launcher.wait(timeout=25)
        except sp.TimeoutExpired:launcher.terminate()
    with (out/'quit.log').open('w') as log:
        try:sp.run([str(root/'tuxoffice'),'wine','C:/tuxoffice/office-probe.exe',app+'-quit'],stdout=log,stderr=log,timeout=15)
        except sp.TimeoutExpired:pass
    sp.run([str(root/'tuxoffice'),'kill'],check=True)
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary),flush=True)
sys.exit(0 if summary['status']=='passed' else 1)
