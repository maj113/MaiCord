"""Independent contextual line-break probes; Windows supplies observations, not this generator."""
import argparse,collections,csv,hashlib,json,os,random,struct,subprocess
from pathlib import Path
def identity(path):
    path=Path(path)
    return dict(path=str(path),bytes=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest())
def generate(root,seed,extensions):
    rng=random.Random(seed);rows=[]
    def add(kind,cps):
        assert 0<len(cps)<=256 and all(0<=cp<=0x10ffff and not 0xd800<=cp<=0xdfff for cp in cps)
        rows.append(dict(kind=kind,scalars=cps))
    sides=[ord(c) for c in 'aA0$%()[]{}!?/\\-"\'']+[0xe9,0x201c,0x201d,0xab,0xbb,0x2026]
    spaces=[0x20,0x9,0xa,0xd,0xa0,0x202f,0x2000,0x2009,0x200b,0x3000]
    marks=[[],[0x301],[0x301,0x327],[0x200c],[0x200d],[0xfe0f,0x301]]
    for a in sides:
        for b in sides:
            for s in spaces:
                for m in marks:add('context',[a,s]+m+[s,b])
    latin=list(range(32,127))+[9,10,13,0xa0,0xe9,0x10d,0x153,0x131,0xdf,0x301,0x308,0x323,0x327,0x34f,0x200c,0x200d,0x2010,0x2011,0x2013,0x2014,0x2018,0x2019,0x201c,0x201d,0x2024,0x2026,0x2028,0x2029,0x202f,0x2060,0xfe0f]
    for _ in range(20000):add('latin_random',[rng.choice(latin) for _ in range(rng.randrange(3,65))])
    mixed=latin+[0x5d0,0x5be,0x627,0x644,0x64e,0x901,0x915,0x94d,0x93e,0x1100,0x1161,0x11a8,0xac00,0xac01,0x4e2d,0x65e5,0x672c,0x3042,0x30a1,0x3001,0x300c,0x300d,0x1f469,0x1f4bb,0x1f3fb,0x1f1e6,0x1f1e7,0x11f02,0x11f12,0x11f42,0x11f13]
    for _ in range(4000):add('mixed_random',[rng.choice(mixed) for _ in range(rng.randrange(3,65))])
    for _ in range(8000):
        cps=[]
        for _ in range(rng.randrange(3,33)):
            cp=rng.randrange(0x10f800);cps.append(cp+0x800 if cp>=0xd800 else cp)
        add('scalar_random',cps)
    for a in sides+[0x1f469,0x1f4bb,0x4e2d,0x5d0,0x627,0x25cc]:
        for modifier in range(0x1f3fb,0x1f400):
            for b in sides+[0x1f469,0x1f4bb,0x4e2d]:
                for m in ([],[0x301],[0x200d,0x301]):add('emoji_context',[a,modifier]+m+[b])
    for n in range(1,16):
        for m in ([],[0x301],[0x200d],[0x20]):add('regional',sum(([0x1f1e6+i%26]+m for i in range(n)),[]))
    for word in ('ภาษาไทยเป็นภาษาที่ไม่มีช่องว่าง','ภาษาไทย ภาษาอังกฤษ','မြန်မာဘာသာ','ພາສາລາວ','ភាសាខ្មែរ'):
        for prefix in ('','a ','(', '\n'):
            for suffix in ('',' b',')','\n'):add('dictionary',list(map(ord,prefix+word+suffix)))
    if extensions!='core':
        for cp in list(range(0x16fe0,0x17000))+list(range(0x18d00,0x18e00)):
            for a,b in ((ord('a'),ord('b')),(0x4e2d,0x4e2d),(0x24,0x25),(0x20,ord('a'))):add('property_edges',[a,cp,b])
        for cp in [0x180b,0x180c,0x180d,0x180f]+list(range(0xfe00,0xfe10))+list(range(0xe0100,0xe01f0)):
            for prefix in ([],[ord('a')],[0x20],[0xd],[0xa],[0xd,0xa],[0x200b],[0x9],[0x2010],[0x1f469]):
                for suffix in ([],[ord('b')],[0x301,ord('b')],[0x25],[0xa]):add('selectors',prefix+[cp]+suffix)
    if extensions=='chains':
        for cp in (0xfe0f,0xe0100,0x180d):
            for count in (1,2,3,4,8):
                for prefix in ([],[ord('a')],[0x20],[0xd],[0xa],[0xd,0xa],[0x200b],[0x9]):
                    for gap in ([],[0x301],[0xfe0e]):
                        for suffix in ([ord('a')],[0x25],[0xa]):add('selector_chains',prefix+([cp]+gap)*count+suffix)
    data=bytearray(b'MCLBPRB1'+struct.pack('<I',len(rows)))
    for i,row in enumerate(rows,1):
        cps=row['scalars'];data+=struct.pack('<II',i,len(cps))+struct.pack('<'+'I'*len(cps),*cps)
    root.mkdir(parents=True,exist_ok=True);(root/'probes.bin').write_bytes(data)
    result=dict(seed=seed,input=identity(root/'probes.bin'),cases=len(rows),groups=dict(collections.Counter(r['kind'] for r in rows)),rows=rows)
    (root/'probes.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps({k:v for k,v in result.items() if k!='rows'}))
def capture(root,exe,name):
    manifest=json.loads((root/'probes.json').read_text());assert identity(root/'probes.bin')==manifest['input']
    with (root/f'{name}.csv').open('w',encoding='utf-8',newline='') as f:
        run=subprocess.run([str(exe),str(root/'probes.bin')],stdout=f,stderr=subprocess.PIPE,text=True)
    (root/f'{name}-capture.json').write_text(json.dumps(dict(executable=identity(exe),input=identity(root/'probes.bin'),exit=run.returncode,stderr=run.stderr),indent=2))
    assert run.returncode==0,run.stderr
def summarize(root,name):
    m=json.loads((root/'probes.json').read_text());capture=json.loads((root/f'{name}-capture.json').read_text())
    assert capture['exit']==0 and identity(capture['executable']['path'])==capture['executable'] and identity(root/'probes.bin')==m['input']==capture['input']
    seen=set();groups={};failures=[]
    for row in csv.DictReader((root/f'{name}.csv').open(encoding='utf-8',newline='')):
        identifier=int(row['source_line']);assert identifier not in seen and 1<=identifier<=m['cases'] and row['has_expected']=='0';seen.add(identifier)
        source=m['rows'][identifier-1];cps=source['scalars'];n=len(cps);starts=[0]
        for cp in cps:starts.append(starts[-1]+(1 if cp<=0xffff else 2))
        assert int(row['scalars'])==n and int(row['utf16_units'])==starts[-1]
        windows,candidate,before,after,white,hyphen=([int(v) for v in row[key].split()] for key in ('windows','candidate','before','after','whitespace','soft_hyphen'))
        assert len(candidate)==len(windows)==n+1 and all(len(v)==starts[-1] for v in (before,after,white,hyphen))
        assert all(v in (0,1,2) for v in candidate) and all(v in (0,1,2,3) for v in before+after) and all(v in (0,1) for v in white+hyphen)
        def boundary(at):return (255,1,0,2)[max(after[at-1] if at else 0,before[at] if at<starts[-1] else 0)]
        assert windows==[boundary(p) for p in starts]
        positions=[i for i in range(1,n) if windows[i]!=255 and candidate[i]!=windows[i]]
        unresolved=sum(windows[i]==255 for i in range(1,n));surrogates=sum(boundary(starts[i]+1) in (1,2) for i,cp in enumerate(cps) if cp>0xffff)
        assert int(row['candidate_differences'])==len(positions) and int(row['checked'])==n-1-unresolved and int(row['unresolved'])==unresolved and int(row['surrogate_breaks'])==surrogates
        group=groups.setdefault(source['kind'],collections.Counter())
        group.update(cases=1,checked=n-1-unresolved,candidate_differences=len(positions),candidate_exact=int(not positions and not unresolved),unresolved=unresolved,surrogate_breaks=surrogates,endpoint_differences=sum(windows[i]!=candidate[i] for i in (0,n)))
        if positions:failures.append(dict(id=identifier,kind=source['kind'],scalars=cps,positions=positions,candidate=candidate,windows=windows))
    assert seen==set(range(1,m['cases']+1))
    report=dict(input=identity(root/'probes.bin'),manifest=identity(root/'probes.json'),csv=identity(root/f'{name}.csv'),executable=capture['executable'],groups=groups,failures=failures)
    (root/f'{name}-comparison.json').write_text(json.dumps(report,indent=2));print(json.dumps(groups))
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=['generate','capture','summarize'])
    p.add_argument('--root',required=True,type=Path);p.add_argument('--seed',type=int,default=0x20260910);p.add_argument('--exe',type=Path);p.add_argument('--name',default='release')
    p.add_argument('--extensions',choices=['core','properties','chains'],default='chains')
    a=p.parse_args()
    if a.mode=='generate':generate(a.root,a.seed,a.extensions)
    elif a.mode=='capture':assert a.exe;capture(a.root,a.exe,a.name)
    else:summarize(a.root,a.name)
if __name__=='__main__':main()
