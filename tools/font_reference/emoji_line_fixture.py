"""Capture and pack Windows emoji line geometry for native differential tests."""
from pathlib import Path
import argparse,hashlib,json,os,struct,subprocess
from embed_fixture import write_embedded_array
def identity(path):
    path=Path(path)
    with path.open('rb') as f:sha=hashlib.file_digest(f,'sha256').hexdigest()
    return dict(path=str(path),bytes=path.stat().st_size,sha256=sha)
def observe(root):
    root.mkdir(parents=True,exist_ok=True);previous=root.parent/'native-emoji-frontend-20260912';research=root.parent/'native-emoji-shaping-20260912'
    exe=root.parent/'native-latin-preprocess-20260910/bin/Release/MaiCordFeaturePlanReference.exe';font=Path('C:/Windows/Fonts/seguiemj.ttf')
    rows=[];cases=[]
    for name in ('observations.json','observations-extra.json'):
        for row in json.loads((research/name).read_text())['rows']:
            if not row['exit']:cases.append((font,row['text'],row['result']['em_bits'],0,False))
    for row in json.loads((previous/'variation-observations.json').read_text())['rows']:
        if not row['exit']:cases.append((Path(row['font']['path']),row['text'],row['result']['em_bits'],row.get('weight',0),True))
    samples=[[0x1f600]*32,[0x20,0x1f600,0x20,0x20],[0x1f600,0x200d],[0x200d,0x1f600],[0x1f469,0x200d,0x2764,0xfe0f,0x200d,0x1f469],[0x31,0xfe0f,0x20e3,0x32,0xfe0f,0x20e3],[0x1f44d,0x1f3fd],[0x2764,0xfe0f],[0x10ffff],[0x20],[0xfe0f],[0x1f600,0x301,0x1f600]]
    for em in (0.75,8.,17.5,21.3,36.25,72.):
        bits=struct.unpack('<I',struct.pack('<f',em))[0]
        for text in samples:cases.append((font,text,bits,0,False))
    env=os.environ.copy();env['LOCALAPPDATA']=str(root/'reference-data');Path(env['LOCALAPPDATA']).mkdir(exist_ok=True)
    for path,text,em,weight,embedded in cases:
        args=[str(exe),str(path),','.join(f'{cp:x}' for cp in text),'default',str(em),'0','0','en-US','edit-layout']
        if weight:args.append(str(weight))
        p=subprocess.run(args,capture_output=True,text=True,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
        row=dict(font=identity(path),text=text,weight=weight,embedded=embedded,exit=p.returncode,result=json.loads(p.stdout) if p.stdout else None,stderr=p.stderr);rows.append(row)
        assert not p.returncode,row
    (root/'line-observations.json').write_text(json.dumps(dict(executable=identity(exe),rows=rows),ensure_ascii=True,indent=2));print('Observed',len(rows),'line cases')
def encode(rows,embedded_only=False):
    groups={}
    for row in rows:
        if embedded_only and not row.get('embedded'):continue
        if row.get('exit'):continue
        r=row['result']
        assert not r['layout_hresult'] and not r['hit_hresult'] and len(r['layout_lines'])==1
        if len(r['layout_glyphs'])>1024 or len(r['layout_clusters'])>256:raise ValueError('Test capacity')
        assert r['layout_bidi']==0
        key=(row['font']['path'],row.get('weight',0),bool(row.get('embedded')))
        groups.setdefault(key,[]).append(row)
    out=bytearray(b'MCEMLIN1'+struct.pack('<I',len(groups)))
    for (name,weight,embedded),cases in groups.items():
        path=Path(name);payload=path.read_bytes() if embedded else str(path).encode('utf-8')
        out+=struct.pack('<IIII',int(embedded),len(payload),len(cases),weight)+payload
        for row in cases:
            r=row['result'];text=''.join(map(chr,row['text'])).encode('utf-8');glyphs=r['layout_glyphs'];maps=r['layout_clusters'];n=len(glyphs)
            points=r.get('point_hits',[]);ranges=r.get('range_hits',[])
            out+=struct.pack('<IIIIII',r['em_bits'],len(text),n,len(maps),len(points),len(ranges))+text
            out+=struct.pack('<'+'H'*n,*glyphs)+struct.pack('<'+'I'*(n*3),*(v for p in r['layout_placement_bits'] for v in p))+struct.pack('<'+'I'*len(maps),*maps)
            line=r['layout_lines'][0];out+=struct.pack('<IIIII',*r['layout_metrics_bits'],line[4],line[1])
            clusters=r['cluster_metrics'];out+=struct.pack('<I',len(clusters))
            for c in clusters:out+=struct.pack('<II',c[0],c[1])
            positions=r['position_hits'];assert len(positions)==(len(maps)+1)*2
            for hit in positions:
                assert hit[5]==hit[7]==hit[10]==hit[12]==0 and hit[11]==1
                out+=struct.pack('<IIIII',hit[2],hit[3],hit[4],hit[6],hit[8])
            for x,y,trailing,inside,m in points:
                out+=struct.pack('<IIII',x,y,m[0]+(m[1] if trailing else 0),inside)
            for first,length,rects in ranges:
                assert len(rects)<=1
                m=rects[0] if rects else [0]*9
                out+=struct.pack('<IIIIIII',first,length,len(rects),m[0],m[1],m[2],m[4])
    return bytes(out),sum(len(v) for v in groups.values())
def pack(root):
    rows=[json.loads(s) for s in (root/'strict-line-observations.jsonl').read_text().splitlines()]
    assert all(not r['exit'] and r['result']['forced_face']==1 and r['result']['verified_face_runs']>0 for r in rows)
    fresh=[r for r in rows if r.get('embedded')]
    manifests=[]
    for name,selected,embedded in [('emoji-line-fixture',fresh,True),('full-lines',rows,False)]:
        binary,count=encode(selected,embedded);path=root/(name+'.bin');path.write_bytes(binary);manifests.append(dict(name=name,cases=count,identity=identity(path)))
        if embedded:
            write_embedded_array('D:/MaiCord/tests/generated/emoji_line_fixture.inc','// Generated Windows geometry observations for first-party synthetic fonts.\n','constexpr u8 emoji_line_fixture[]',binary)
    (root/'line-fixtures.json').write_text(json.dumps(manifests,indent=2));print(json.dumps(manifests,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['observe','pack']);p.add_argument('root',type=Path);a=p.parse_args();(observe if a.action=='observe' else pack)(a.root)
