"""Extend the frozen matching matrix with real private default-face collections."""
from pathlib import Path
import argparse,json,subprocess
from font_catalog_compare import identity
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--reference',type=Path,required=True);a=p.parse_args()
    original=json.loads(a.reference.read_text());files={}
    for family in ['Bahnschrift','Georgia','Yu Gothic UI']:
        group=next(g for g in original['groups'] if g['family']==family and not g.get('custom'))
        face=next(group['faces'][str(i)] for i in group['base'] if not group['faces'][str(i)]['simulations']);files[family]=face['file']
    exe=a.root/'bin/Release/MaiCordFontMatchingReference.exe';groups=list(original['groups']);observations=[]
    for i,font in enumerate(files.values()):
        assert identity(font['path'])==font
        run=subprocess.run([str(exe),'--families',font['path']],capture_output=True,text=True);assert run.returncode==0,run.stdout
        families=[r for r in map(json.loads,run.stdout.splitlines()) if r['kind']=='collection_family'];assert len(families)==1;family=families[0]['name']
        names=a.root/f'default-names-{i}.jsonl';names.write_text(run.stdout)
        run=subprocess.run([str(exe),family,font['path']],capture_output=True,text=True);assert run.returncode==0,run.stdout
        raw=a.root/f'default-matching-{i}.jsonl';raw.write_text(run.stdout);records=list(map(json.loads,run.stdout.splitlines()));faces={str(r['id']):r for r in records if r['kind']=='face'}
        for face in faces.values():face['file']=identity(face['path'])
        base=next(r['candidates'] for r in records if r['kind']=='family');assert len([f for f in faces.values() if not f['simulations']])==1
        groups.append(dict(family=family,custom=font['path'],raw=identity(raw),faces=faces,base=base,queries=[r for r in records if r['kind']=='query']))
        observations.append(dict(font=font,names=identity(names),matching=identity(raw),family=family))
    result=dict(reference=identity(a.reference),executable=identity(exe),groups=groups,added=observations)
    (a.root/'matching-reference.json').write_text(json.dumps(result,indent=2));print([r['family'] for r in observations],len(groups),'groups')
if __name__=='__main__':main()
