"""First-party direction probes for the pending native bold outline provider."""
import argparse,json,math,subprocess
from pathlib import Path
from bold_geometry_oracle import make_font,decode,identity

def run(root,exe):
    root.mkdir(parents=True,exist_ok=True);shapes={};vectors={}
    for degree in range(360):
        # Even deltas keep the middle points exactly collinear on the integer grid.
        x=round(math.cos(math.radians(degree))*1024)*2
        y=round(math.sin(math.radians(degree))*1024)*2
        name=f'direction_{degree}';vectors[name]=(x,y)
        shapes[name]=[[(0,0),(x//2,y//2),(x,y),(x+2048,y),(x//2+2048,y//2),(2048,0)]]
    records=[]
    for units in [16,51,512,1000,1024,16384]:
        path=root/f'directions-{units}.ttf';order=make_font(path,units,shapes);modes={}
        for mode in [0,1]:
            p=subprocess.run([str(exe),str(path),f'--simulate={mode}','--instructions-dump'],capture_output=True,text=True,timeout=60)
            (root/f'directions-{units}-{mode}.txt').write_text(p.stdout+p.stderr)
            assert p.returncode in ([0] if not mode else [0,6])
            rows=[list(map(int,s.split('=',1)[1].split(','))) for s in p.stdout.splitlines() if s.startswith('gvar_outline=')]
            assert len(rows)==len(order)
            modes[str(mode)]={name:decode(row,units) for name,row in zip(order,rows)}
        results={}
        for name,vector in vectors.items():
            a,b=modes['0'][name],modes['1'][name]
            assert [c['kind'] for c in a]==[c['kind'] for c in b]
            assert len(a)==7 and a[-1]['kind']==5
            results[name]=dict(vector=vector,displacements=[tuple(round((y['design_coordinates'][i]-x['design_coordinates'][i])*64)/64 for i in range(2)) for x,y in zip(a,b) if x['kind']!=5])
        records.append(dict(font=identity(path),units=units,directions=results))
    data=dict(reference=identity(exe),cases=records)
    (root/'observations.json').write_text(json.dumps(data,indent=2))
    print(len(records),'fonts;',len(vectors),'directions each')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',required=True,type=Path);a=p.parse_args();run(a.root.resolve(),a.exe.resolve())
