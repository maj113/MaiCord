"""QA-only WWS matching observations and candidate-model diagnostics."""
from pathlib import Path
import argparse,json,hashlib,subprocess
def identity(path):
    path=Path(path).resolve();data=path.read_bytes();return dict(path=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
def main():
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--scope',type=Path,required=True);p.add_argument('--application',type=Path,required=True);a=p.parse_args()
    a.root.mkdir(exist_ok=True,parents=True);scope=json.loads(a.scope.read_text());tasks=[];known=set()
    for item in scope['font_choices']:
        if item['file']:tasks.append((item['family'],str(Path('D:/MaiCord/assets/fonts')/item['file'])))
        else:tasks.append((item['family'],None));known.add(item['family'])
    bundled={f['family'] for f in scope['bundled']}
    for line in a.application.read_text().splitlines():
        if not line.startswith('{'):continue
        row=json.loads(line)
        if row['kind']=='face' and row['family'] not in known|bundled:tasks.append((row['family'],None));known.add(row['family'])
    exe=a.root/'bin/Release/MaiCordFontMatchingReference.exe';groups=[];diff=[];failures=[];missing=[]
    for i,(family,path) in enumerate(tasks):
        process=subprocess.run([str(exe),family]+([path] if path else []),capture_output=True,text=True)
        raw=a.root/f'family-{i:02d}.jsonl';raw.write_text(process.stdout)
        if process.returncode:failures.append(dict(family=family,path=path,exit=process.returncode,stdout=process.stdout));continue
        rows=[json.loads(l) for l in process.stdout.splitlines()]
        if rows==[dict(kind='missing_family',name=family)]:missing.append(dict(family=family,raw=identity(raw)));continue
        faces={r['id']:r for r in rows if r['kind']=='face'};base=next(r['candidates'] for r in rows if r['kind']=='family')
        group=dict(family=family,custom=path,raw=identity(raw),faces=faces,base=base,queries=[r for r in rows if r['kind']=='query']);groups.append(group)
        for face in faces.values():face['file']=identity(face['path'])
        for query in group['queries']:
            weight,width,style=query['request'];q=[(width-5)*1100,style*700,(weight-400)*5];candidates=[]
            for id in base:
                f=faces[id]
                if f['simulations']:continue
                w,s,t=f['properties'];sim=0
                if style and t==0:t=1;sim|=2
                if weight>=600 and w<=550 and weight>w+100:w=700;sim|=1
                v=[(s-5)*1100,t*700,(w-400)*5];d=[x-y for x,y in zip(v,q)]
                key=(sum(x*x for x in d),-sum(x*y for x,y in zip(v,q)),abs(d[0]),abs(d[1]),abs(d[2]),sim.bit_count(),abs(f['properties'][0]-weight),id)
                candidates.append((key,f,sim,[w,s,t]))
            _,f,sim,props=min(candidates);expected=faces[query['matches'][0]]
            same=f['file']['sha256']==expected['file']['sha256'] and f['index']==expected['index'] and f['axes']==expected['axes'] and sim==expected['simulations']
            if not same:diff.append(dict(family=family,request=query['request'],expected=expected,actual=dict(base=f,simulations=sim,properties=props)))
    result=dict(executable=identity(exe),scope=identity(a.scope),application=identity(a.application),groups=groups,queries=sum(len(g['queries']) for g in groups),failures=failures,missing_families=missing,differences=diff)
    (a.root/'capture.json').write_text(json.dumps(result,indent=2));print(dict(groups=len(groups),queries=result['queries'],failures=failures,missing_families=[m['family'] for m in missing],differences=len(diff)))
    print(json.dumps(diff[:3],indent=2))
if __name__=='__main__':main()
