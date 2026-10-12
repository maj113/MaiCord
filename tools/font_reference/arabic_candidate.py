"""Capture and compare synthetic Arabic cases through the application boundary."""
import argparse,collections,json,os,struct,subprocess
from pathlib import Path
from arabic_app_oracle import samples
from app_text_boundary import identity

def prepare(root):
    root.mkdir(parents=True,exist_ok=True);texts=[''.join(map(chr,s)) for s in samples()]
    texts += [' '+texts[0],texts[0]+' ',texts[0]+'  ','\u0628\u0628\u034f\u0628','\u0628\u0628 123','Latin \u0628\u0628','\u0628\u0628\n\u0628\u0628']
    cases=[];data=bytearray(b'MCTEXTB1'+bytes(4))
    for profile in range(21):
      for sample,text in enumerate(texts):
       for style in [0,1]:
        for independent in [False,True]:
         for width in [1000000.,120.]:
          c=dict(id=len(cases),profile=profile,sample=sample,text=text,style=style,independent=independent,width=width,height=1000000.);cases.append(c)
          raw=text.encode();data+=struct.pack('<IIIIffI',c['id'],profile,style,independent,width,c['height'],len(raw))+raw
    struct.pack_into('<I',data,8,len(cases));(root/'corpus.bin').write_bytes(data)
    (root/'corpus.json').write_text(json.dumps(dict(cases=cases),ensure_ascii=False,indent=2),encoding='utf-8');print(len(cases),'cases')

def capture(root,exe,name):
    data=root/(name+'-data');data.mkdir(exist_ok=True);env=os.environ.copy();env['LOCALAPPDATA']=str(data)
    output=root/(name+'.jsonl')
    with output.open('wb') as out:p=subprocess.run([str(exe),str(root/'corpus.bin')],stdout=out,stderr=subprocess.PIPE,env=env,creationflags=0x08000000)
    result=dict(executable=identity(exe),input=identity(root/'corpus.bin'),output=identity(output),exit=p.returncode,stderr=p.stderr.decode(errors='replace'))
    (root/(name+'-capture.json')).write_text(json.dumps(result,indent=2));assert p.returncode==0,result;print(name,'captured')

def read(path):
    rows=[json.loads(line) for line in path.read_text().splitlines() if line.startswith('{')];assert rows[-1]['kind']=='summary' and rows[-1]['ok']==1 and not rows[-1]['app_failures']
    faces={};files={};cases={};dispatch={}
    for row in rows:
        if row['kind']=='face':
            file=files.setdefault(row['path'],identity(row['path']));faces[row['id']]=[file['sha256'],row['index'],row['simulations'],row['axes']]
        elif row['kind']=='case':
            for run in row.get('runs',[]):run[0]=faces[run[0]]
            assert row['id'] not in cases;cases[row['id']]=row
        elif row['kind']=='native_arabic_dispatch':
            assert row['id'] not in dispatch;dispatch[row['id']]=row
    assert len(cases)==rows[-1]['cases'];return cases,dispatch,list(files.values())

def compare(root,reference,candidate,output):
    a,_,af=read(reference);b,used,bf=read(candidate);assert a.keys()==b.keys()
    meta=json.loads((root/'corpus.json').read_text(encoding='utf-8'))['cases'];assert len(meta)==len(a)
    differences=[]
    for i in a:
        if a[i]!=b[i]:differences.append(dict(case=i,sample=meta[i]['sample'],native=i in used,fields=[k for k in a[i] if a[i].get(k)!=b[i].get(k)],reference=a[i],candidate=b[i]))
    for value in used.values():assert value.get('shaped')==value.get('measured')==1
    result=dict(reference=identity(reference),candidate=identity(candidate),cases=len(a),exact=len(a)-len(differences),native_shaped=len(used),native_measured=len(used),
        native_profiles=dict(collections.Counter(str(meta[i]['profile']) for i in used)),dispatch_ids=sorted(used),differences=differences,reference_fonts=af,candidate_fonts=bf)
    output.write_text(json.dumps(result,indent=2));print({k:result[k] for k in ['cases','exact','native_shaped','native_measured','native_profiles']})

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('action',choices=['prepare','capture','compare']);p.add_argument('root',type=Path);p.add_argument('--exe',type=Path);p.add_argument('--name');p.add_argument('--reference',type=Path);p.add_argument('--candidate',type=Path);p.add_argument('--out',type=Path);a=p.parse_args();a.root=a.root.resolve()
    if a.action=='prepare':prepare(a.root)
    elif a.action=='capture':capture(a.root,a.exe.resolve(),a.name)
    else:compare(a.root,a.reference.resolve(),a.candidate.resolve(),a.out.resolve())
