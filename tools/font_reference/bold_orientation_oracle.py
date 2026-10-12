"""Read per-contour flags from disposable reference processes, never the app."""
import argparse,json,re,subprocess
from pathlib import Path
from bold_geometry_oracle import identity

def run(root,model,probes=False):
    root.mkdir(parents=True,exist_ok=True);data=json.loads(model.read_text());exe=Path(data['reference']['path'])
    assert identity(exe)==data['reference']
    cdb=Path('C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe')
    symbols='srv*'+str(root/'symbols')+'*https://msdl.microsoft.com/download/symbols'
    command='bp DWrite!fsg_Embold+0xf5; g; .printf "DIRECTIONS count=%d\\n", wo(@r14+0x50); r @$t0=0; .while (@$t0 < wo(@r14+0x50)) { .printf "%d,", by(poi(@r14+0x58)+@$t0); r @$t0=@$t0+1 }; .echo END; q'
    if probes:
        command='bp DWrite!Misoriented ".printf \\"PROBE c=%d d=%d i=%d x=%d y=%d\\\\n\\", @ecx, @edx, @r8d, @r9d, (@r9>>0x20); gc"; bp DWrite!Misoriented+0x422 ".printf \\"RESULT crossings=%d turn=%d\\\\n\\", @ecx, dwo(@rsp+0x88); gc"; '+command
    cases=[]
    for row in data['files']:
        font=Path(row['font']['path']);assert identity(font)==row['font']
        for glyph in row['analysis']['different_glyphs']:
            p=subprocess.run([str(cdb),'-y',symbols,'-c',command,str(exe),str(font),'--simulate=1','--points',f'--instructions-state={glyph}'],capture_output=True,text=True,timeout=60,creationflags=0x08000000)
            log=root/f'{font.stem}-{glyph}.txt';log.write_text(p.stdout+p.stderr)
            match=re.search(r'^DIRECTIONS count=(\d+)\n([0-9,]+)END',p.stdout,re.M)
            assert p.returncode==0 and match,(font,glyph,p.returncode)
            flags=list(map(int,match[2].rstrip(',').split(',')));assert len(flags)==int(match[1]) and all(f in [0,1] for f in flags)
            item=dict(font=row['font'],glyph=glyph,directions=flags,log=identity(log))
            if probes:
                matches=re.findall(r'^PROBE c=(-?\d+) d=(-?\d+) i=(-?\d+) x=(-?\d+) y=(-?\d+)\nRESULT crossings=(-?\d+) turn=(-?\d+)',p.stdout,re.M)
                item['probes']=[dict(zip(['contour','direction','index','x','y','crossings','turn'],map(int,m))) for m in matches]
                assert len(matches)>=len(flags)*2,(font,glyph,len(matches))
            cases.append(item)
            print(font.stem,glyph,flags,flush=True)
    (root/'observations.json').write_text(json.dumps(dict(reference=data['reference'],debugger=identity(cdb),model=identity(model),cases=cases),indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--model',required=True,type=Path);p.add_argument('--probes',action='store_true');a=p.parse_args();run(a.root.resolve(),a.model.resolve(),a.probes)
