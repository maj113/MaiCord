"""Observe itemized emoji/non-visual paragraphs and pack native geometry cases."""
from pathlib import Path
import argparse,hashlib,json,os,random,struct,subprocess
from embed_fixture import write_embedded_array

def identity(path):
    path=Path(path)
    with path.open('rb') as stream:sha=hashlib.file_digest(stream,'sha256').hexdigest()
    return dict(path=str(path),bytes=path.stat().st_size,sha256=sha)

def capture(root):
    prior=root.parent/'native-emoji-lines-20260912';research=root.parent/'native-emoji-shaping-20260912'
    old=list(map(json.loads,(prior/'strict-line-observations.jsonl').read_text().splitlines()));font=old[0]['font']
    visibility=list(map(json.loads,(root/'visibility-observations.jsonl').read_text().splitlines()))
    assert len(visibility)==5 and all(r['nonvisual_ranges']==visibility[0]['nonvisual_ranges'] and r['scalars']==1112064 and r['nonvisual']==206 for r in visibility)
    hidden=[cp for a,b in visibility[0]['nonvisual_ranges'] for cp in range(a,b+1)]
    cases=[dict(font=r['font'],text=r['text'],weight=r['weight'],embedded=r['embedded'],em=r['result']['em_bits']) for r in old[:125]]
    tags=[r['text'] for r in map(json.loads,(research/'corpus-reference.jsonl').read_text().splitlines()) if r['exit']]
    assert len(tags)==3
    def add(text,em=17.5,selected=font,embedded=False,weight=0):cases.append(dict(font=selected,text=text,weight=weight,embedded=embedded,em=struct.unpack('<I',struct.pack('<f',em))[0]))
    for cp in hidden:
        for text in ([cp],[0x1f600,cp,0x1f600],[0x1f600,32,cp],[cp,32,0x1f600]):add(text)
    for text in tags:
        for sample in (text,[0x1f600]+text,text+[0x1f600],text+text,[32]+text+[32],[0xe0067]+text,text+[0x301],text+[10]+text):
            for em in (.75,8,17.5,21.3,36.25,72):add(sample,em)
    for text in ([0xe0067,0xe007f],[0xe0067,0x301],[0x1d173,0x1d174],[0x1f600,13,10,0x1f600],[13,10],[10],[0x1f600,9,0x1f600],[0x1f600,10],[0x1f600,0xe0067,32,32]):add(text)
    # Synthetic fonts prove that no visible glyph/lookup is emitted for controls.
    for row in old[:125]:
        if row['embedded']:
            for text in ([0xe0067],[0,0xe0067,0x2060],[0xe0067]+row['text']+[0xe007f]):add(text,17.5,row['font'],True,row['weight'])
    rng=random.Random(0x454d4954);atoms=[[0x1f600],[0x1f44d,0x1f3fd],[0x31,0xfe0f,0x20e3],[0x2764,0xfe0f],[32],[0x200b],[0xe0067,0xe007f],[0],[0x2060],tags[0]]
    for i in range(1024):add([cp for _ in range(rng.randrange(1,10)) for cp in rng.choice(atoms)],rng.choice([8.,17.5,21.3,36.25]))
    exe=root/'bin/Release/MaiCordEmojiItemsReference.exe';env=os.environ.copy();env['LOCALAPPDATA']=str(root/'reference-data');Path(env['LOCALAPPDATA']).mkdir(exist_ok=True)
    with (root/'item-observations.jsonl').open('w') as stream:
        for i,c in enumerate(cases):
            args=[str(exe),c['font']['path'],','.join(f'{cp:x}' for cp in c['text']),'default',str(c['em']),'0','0','en-US','edit-layout']
            if c['weight']:args.append(str(c['weight']))
            p=subprocess.run(args,capture_output=True,text=True,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
            row=c|dict(index=i,exit=p.returncode,result=json.loads(p.stdout) if p.stdout else None,stderr=p.stderr);stream.write(json.dumps(row,ensure_ascii=True)+'\n')
            assert not row['stderr'],row
            if p.returncode:
                # Preserve hidden-item font substitution as a font-selection
                # finding, never as a same-selected-face geometry expectation.
                assert c['embedded'] and row['result'].get('actual_glyph_count')!=row['result'].get('expected_glyph_count') and row['result'].get('actual_glyph_count'),row
            else:
                assert row['result']['verified_face_runs']>0,row
                assert all(v==0 for k,v in row['result'].items() if k=='hresult' or k.endswith('_hresult')),row
            if i%500==0:print('Observed itemized paragraphs',i,flush=True)
    (root/'item-reference-inputs.json').write_text(json.dumps(dict(executable=identity(exe),font_inputs=list({r['font']['path']:r['font'] for r in cases}.values()),cases=len(cases)),indent=2))
    print('Captured',len(cases),'itemized cases')

def encode(rows):
    groups={}
    for row in rows:groups.setdefault((row['font']['path'],row.get('weight',0),bool(row.get('embedded'))),[]).append(row)
    out=bytearray(b'MCEMPAR1'+struct.pack('<I',len(groups)))
    def words(*values):out.extend(struct.pack('<'+'I'*len(values),*values))
    for (name,weight,embedded),cases in groups.items():
        path=Path(name);payload=path.read_bytes() if embedded else str(path).encode('utf-8');words(int(embedded),len(payload),len(cases),weight);out.extend(payload)
        for row in cases:
            r=row['result'];text=''.join(map(chr,row['text'])).encode('utf-8');glyphs=r['layout_glyphs'];maps=r['layout_clusters'];points=r.get('point_hits',[]);ranges=r.get('range_hits',[]);runs=r.get('layout_runs')
            words(r['em_bits'],len(text),len(glyphs),len(maps),len(points),len(ranges),len(runs) if runs is not None else 0xffffffff,len(r['layout_lines']));out.extend(text)
            out.extend(struct.pack('<'+'H'*len(glyphs),*glyphs));words(*(v for p in r['layout_placement_bits'] for v in p));words(*maps)
            words(*r['layout_metrics_bits'],r['layout_lines'][0][3],r['layout_lines'][0][4],len(r['cluster_metrics']))
            for c in r['cluster_metrics']:words(c[0],c[1])
            for hit in r['position_hits']:words(*hit)
            for x,y,trailing,inside,m in points:words(x,y,m[0]+(m[1] if trailing else 0),inside)
            for first,length,rects in ranges:
                words(first,length,len(rects))
                for m in rects:words(*m)
            if runs is not None:
                for run in runs:words(*run)
    return bytes(out)

def capture_extra(root):
    original=list(map(json.loads,(root/'item-observations.jsonl').read_text().splitlines()));font=original[0]['font']
    intervals=json.loads((root/'visibility-observations.jsonl').read_text().splitlines()[0])['nonvisual_ranges']
    cases=[]
    for a,b in intervals:
        for cp in range(a,b+1):
            for tail in ([0xfe0f],[0xe0100],[0x180b],[0xfe0f,0xfe0f]):cases.append([cp]+tail)
    for control in ([13,10],[13,0xfe0f,10],[13,10,0xfe0f],[13,0xfe0f,10,0xfe0f],[13,10,0xe0100],[13,0xe0100,10],[10,0xfe0f],[9,0xfe0f]):
        for sample in (control,[0x1f600]+control,control+[0x1f600],[0x1f600]+control+[0x1f600]):cases.append(sample)
    exe=root/'bin/Release/MaiCordEmojiItemsReference.exe';env=os.environ.copy();env['LOCALAPPDATA']=str(root/'reference-data')
    with (root/'selector-observations.jsonl').open('w') as stream:
        for i,t in enumerate(cases):
            args=[str(exe),font['path'],','.join(f'{cp:x}' for cp in t),'default','1099694080','0','0','en-US','edit-layout']
            p=subprocess.run(args,capture_output=True,text=True,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
            row=dict(font=font,text=t,weight=0,embedded=False,em=1099694080,index=len(original)+i,exit=p.returncode,result=json.loads(p.stdout) if p.stdout else None,stderr=p.stderr)
            stream.write(json.dumps(row,ensure_ascii=True)+'\n');assert not p.returncode and not p.stderr and row['result']['verified_face_runs']>0,row
            assert all(v==0 for k,v in row['result'].items() if k=='hresult' or k.endswith('_hresult')),row
            if i%250==0:print('Observed control/selector cases',i,flush=True)
    (root/'selector-reference-inputs.json').write_text(json.dumps(dict(executable=identity(exe),font_inputs=[font],cases=len(cases)),indent=2));print('Captured',len(cases),'selector cases')

def pack(root):
    rows=list(map(json.loads,(root/'item-observations.jsonl').read_text().splitlines()));old=list(map(json.loads,(root.parent/'native-emoji-lines-20260912/strict-line-observations.jsonl').read_text().splitlines()))
    if (root/'selector-observations.jsonl').exists():rows+=list(map(json.loads,(root/'selector-observations.jsonl').read_text().splitlines()))
    selected=[];excluded=[]
    for row in rows:
        r=row['result'];reason=None
        if row['exit']:reason='Windows selected another face for non-visual items'
        elif 0xad in row['text']:reason='discretionary soft hyphen'
        elif any(run[4] for run in r['layout_runs']):reason='resolved bidi item requires bidi assembly'
        elif any(cp==0x200f or 0x202a<=cp<=0x202e or 0x2066<=cp<=0x2069 for cp in row['text']):reason='bidi formatting control requires bidi assembly'
        if reason:excluded.append(dict(index=row['index'],text=row['text'],reason=reason))
        else:selected.append(row)
    # The first 125 rows are observed fresh; the rest are the verified visible corpus.
    selected+=old[125:]
    records=[]
    builtin=[r for r in selected if r['embedded']]+[r for r in selected if not r['embedded'] and r.get('index',0)>=125][:64]
    builtin += [r for r in selected if r.get('index',0)>=125 and 0x1f3f4 in r['text']][:12]
    for name,data in [('emoji-paragraph-fixture',builtin),('full-paragraphs',selected)]:
        binary=encode(data);path=root/(name+'.bin');path.write_bytes(binary);records.append(dict(name=name,cases=len(data),identity=identity(path)))
        if name=='emoji-paragraph-fixture':
            write_embedded_array('D:/MaiCord/tests/generated/emoji_paragraph_fixture.inc','// Generated Windows itemized geometry for first-party synthetic fonts.\n','constexpr u8 emoji_paragraph_fixture[]',binary)
    (root/'paragraph-fixtures.json').write_text(json.dumps(dict(fixtures=records,fresh_selected=len(selected)-len(old[125:]),prior_visible=len(old[125:]),excluded=excluded),ensure_ascii=True,indent=2));print(json.dumps(records,indent=2),'Excluded',len(excluded),'requiring other layout policy')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['capture','selectors','pack']);p.add_argument('root',type=Path);a=p.parse_args();dict(capture=capture,selectors=capture_extra,pack=pack)[a.action](a.root)
