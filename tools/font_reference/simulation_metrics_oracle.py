"""Record style-simulation design metrics for every observed app font/face."""
import argparse,collections,json,subprocess
from pathlib import Path
from bold_geometry_oracle import identity


def parse(output):
    faces={};face=None
    for line in output.splitlines():
        if line.startswith('face='):
            face=int(line.split('=')[1]);faces[face]={'glyphs':[]}
        elif line.startswith('units_per_em='):faces[face]['units']=int(line.split('=')[1])
        elif line.startswith('reference_metrics='):
            row=list(map(int,line.split('=')[1].split(',')));assert row[0]==len(faces[face]['glyphs'])
            faces[face]['glyphs'].append(row[1:])
    assert faces and all(f.get('units') and f['glyphs'] for f in faces.values())
    return faces


def run(root,exe,inventory):
    root.mkdir(parents=True,exist_ok=True);records=[]
    summary=json.loads(inventory.read_text(encoding='utf-8'))
    for index,font in enumerate(summary['files']):
        path=Path(font['path']);assert identity(path)['sha256']==font['sha256']
        modes={}
        for mode in range(4):
            p=subprocess.run([str(exe),str(path),f'--simulate={mode}','--metrics-dump'],capture_output=True,text=True,timeout=60)
            (root/f'{index:02d}-{path.stem}-{mode}.txt').write_text(p.stdout+p.stderr)
            assert p.returncode==0,(path,mode,p.returncode)
            modes[str(mode)]=parse(p.stdout)
        assert all(modes[m].keys()==modes['0'].keys() for m in modes)
        records.append(dict(font=identity(path),modes=modes))
    data=dict(reference=identity(exe),inventory=identity(inventory),files=records)
    (root/'observations.json').write_text(json.dumps(data,separators=(',',':')))
    results=analyse(data);(root/'summary.json').write_text(json.dumps(results,indent=2));print(json.dumps(results,indent=2))


def analyse(data):
    counts=collections.Counter();examples=[];deltas=collections.Counter()
    for file in data['files']:
        modes=file['modes']
        for face,base in modes['0'].items():
            units=base['units'];amount=(units+25)//50
            counts['faces']+=1
            assert all(modes[m][face]['units']==units and len(modes[m][face]['glyphs'])==len(base['glyphs']) for m in modes)
            for glyph,plain in enumerate(base['glyphs']):
                bold=modes['1'][face]['glyphs'][glyph];counts['glyphs']+=1
                diff=tuple(b-a for a,b in zip(plain,bold));deltas[diff]+=1
                width=plain[1]-plain[0]-plain[2];height=plain[4]-plain[3]-plain[5]
                expected=(0,amount if plain[1] else 0,0 if plain[1] else -amount,
                          0 if plain[4] else -amount,amount if plain[4] else 0,0,
                          amount if plain[4] else 0) if width and height else (0,)*7
                if diff!=expected:
                    counts['bold_rule_differences']+=1
                    if len(examples)<32:examples.append(dict(file=file['font']['path'],face=face,glyph=glyph,units=units,base=plain,bold=bold,expected_delta=expected))
                for m in ['2','3']:
                    previous=modes[str(int(m)-2)][face]['glyphs'][glyph]
                    styled=modes[m][face]['glyphs'][glyph]
                    if any(styled[j]!=previous[j] for j in [1,3,4,5,6]):counts['oblique_nonbearing_differences']+=1
                    ymin=previous[6]+previous[5]-previous[4];ymax=previous[6]-previous[3]
                    left=ymin*87//256;right=ymax*87//256
                    if not width or not height:left=right=0
                    expected_oblique=previous[:];expected_oblique[0]+=left;expected_oblique[2]-=right
                    if styled!=expected_oblique:
                        counts['oblique_bearing_differences']+=1
                        if len(examples)<32:examples.append(dict(file=file['font']['path'],face=face,glyph=glyph,mode=m,base=previous,styled=styled,expected=expected_oblique))
    return dict(counts=dict(counts),bold_deltas=[dict(delta=d,count=n) for d,n in deltas.most_common()],examples=examples)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',required=True,type=Path);p.add_argument('--inventory',required=True,type=Path)
    a=p.parse_args();run(a.root.resolve(),a.exe.resolve(),a.inventory.resolve())
