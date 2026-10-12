"""Capture first-party undefined-prep-call fonts for fractional bold policy."""
import argparse,json,subprocess
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables.ttProgram import Program
from bold_geometry_oracle import make_font,identity,decode

def run(root,exe):
    root.mkdir(parents=True,exist_ok=True);cases=[]
    for units in [16,51,512,1000,2048,16384]:
        path=root/f'fractional-{units}.ttf';order=make_font(path,units)
        font=TTFont(path,recalcTimestamp=False);font['prep']=newTable('prep');font['prep'].program=Program();font['prep'].program.fromBytecode(b'\xb0\x00\x2b');font['maxp'].maxStackElements=64
        # Keep an explicit duplicated closing endpoint; TTGlyphPen normally
        # removes it. Its incoming zero vector must survive widening.
        for name in ['duplicate','duplicate_reverse']:
            glyph=font['glyf'][name];glyph.coordinates.append(glyph.coordinates[0]);glyph.flags.append(glyph.flags[0]);glyph.endPtsOfContours[-1]+=1
        font.save(path)
        modes={};logs={}
        for mode in [0,1]:
            p=subprocess.run([str(exe),str(path),f'--simulate={mode}','--points','--instructions-dump'],capture_output=True,text=True,timeout=60,creationflags=0x08000000)
            log=root/f'fractional-{units}-{mode}.txt';log.write_text(p.stdout+p.stderr)
            assert p.returncode in ([0] if mode==0 else [0,6]),(units,mode,p.returncode)
            assert 'instruction_prepare_missing_function=1' in p.stdout
            rows=[list(map(int,s.split('=',1)[1].split(','))) for s in p.stdout.splitlines() if s.startswith('gvar_outline=')]
            assert len(rows)==len(order);modes[str(mode)]={name:decode(row,units) for name,row in zip(order,rows)};logs[str(mode)]=identity(log)
        cases.append(dict(font=identity(path),units=units,fractional_halves=True,outlines=modes,logs=logs))
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),cases=cases),indent=2))
    print(len(cases),'fractional-policy fonts captured')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);a=p.parse_args();run(a.root.resolve(),a.exe.resolve())
