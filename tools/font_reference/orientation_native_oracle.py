"""Compare native simple-contour classification with frozen Windows flags."""
import argparse,json,re,subprocess
from pathlib import Path
from bold_geometry_oracle import identity

def run(root,source,exe):
    root.mkdir(parents=True,exist_ok=True);data=json.loads(source.read_text());rows=[];contours=0
    for case in data['cases']:
        font=Path(case['font']['path']);assert identity(font)==case['font']
        # The integrated bold path now widens its points. Use the unsimulated
        # decode here to inspect original simple contour directions separately.
        p=subprocess.run([str(exe),str(font),'--simulate=0','--points',f'--instructions-state={case["glyph"]}'],capture_output=True,text=True,timeout=60,creationflags=0x08000000)
        log=root/f'{font.stem}-{case["glyph"]}.txt';log.write_text(p.stdout+p.stderr);assert p.returncode in [0,6],(font,case['glyph'],p.returncode)
        match=re.search(r'^native_contour_directions=([0-9,]+)$',p.stdout,re.M);assert match
        glyph,status,work,count,*flags=map(int,match[1].split(','));assert status==0 and glyph==case['glyph'] and count==len(case['directions']) and flags==case['directions'],(font,glyph,status,flags,case['directions'])
        transient=int(re.search(r'^instruction_prepare_missing_function=(\d+)$',p.stdout,re.M)[1]);retained=int(re.search(r'^instruction_instance_prep_recovered=(\d+)$',p.stdout,re.M)[1]);assert transient==retained
        rows.append(dict(glyph=glyph,font=case['font'],directions=flags,work=work,prep_recovered=bool(retained),instance_bytes=int(re.search(r'^instruction_instance_bytes=(\d+)$',p.stdout,re.M)[1]),log=identity(log)));contours+=count
    result=dict(native=identity(exe),reference=identity(source),glyphs=len(rows),contours=contours,cases=rows)
    (root/'observations.json').write_text(json.dumps(result,indent=2));print(len(rows),'glyphs;',contours,'contours exact; instance bytes',sorted({r['instance_bytes'] for r in rows}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--reference',type=Path,required=True);p.add_argument('--exe',type=Path,required=True);a=p.parse_args();run(a.root.resolve(),a.reference.resolve(),a.exe.resolve())
