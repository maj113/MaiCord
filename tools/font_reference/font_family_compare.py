from pathlib import Path
import argparse,json,subprocess,hashlib
def identity(path):
    path=Path(path).resolve();data=path.read_bytes();return dict(path=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
def main():
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--reference',type=Path,required=True);p.add_argument('--config',choices=['Release','Debug'],required=True);p.add_argument('--verify',action='store_true');a=p.parse_args()
    capture=json.loads(a.reference.read_text());files={};exe=a.root/f'MaiCordFontFamilyTests.exe'
    for g in capture['groups']:
        for i in g['base']:
            f=g['faces'][str(i)]
            if f['simulations']:continue
            ident=(f['file']['sha256'],f['index']);entry=files.setdefault(ident,dict(font=f['file'],index=f['index'],family=g['family'],properties=[]))
            assert entry['family']==g['family']
            if f['properties'] not in entry['properties']:entry['properties'].append(f['properties'])
    # The profile's old requested name is missing; capture its repaired family
    # from this file's fresh Windows collection rather than guessing the repair.
    path=Path('C:/Windows/Fonts/ARLRDBD.TTF');ref=a.root/'bin/Release/MaiCordFontMatchingReference.exe'
    process=subprocess.run([str(ref),'--families',str(path)],capture_output=True,text=True);assert process.returncode==0
    observed=[json.loads(l) for l in process.stdout.splitlines()];name=next(r['name'] for r in observed if r['kind']=='collection_family');physical=[r for r in observed if r['kind']=='face' and not r['simulations']];assert len(physical)==1
    files[(identity(path)['sha256'],0)]=dict(font=identity(path),index=0,family=name,properties=[physical[0]['properties']])
    process=subprocess.run([str(ref),name],capture_output=True,text=True);assert process.returncode==0
    raw=a.root/'arial-matching-reference.jsonl'
    if a.verify:assert raw.read_text()==process.stdout
    else:raw.write_text(process.stdout)
    matching=[json.loads(l) for l in process.stdout.splitlines()];extra_faces={r['id']:r for r in matching if r['kind']=='face'}
    for face in extra_faces.values():face['file']=identity(face['path'])
    extra=dict(family=name,custom=None,raw=identity(raw),faces=extra_faces,base=next(r['candidates'] for r in matching if r['kind']=='family'),queries=[r for r in matching if r['kind']=='query'])
    extended=dict(capture);extended['groups']=capture['groups']+[extra];extended['queries']=capture['queries']+len(extra['queries']);extended['appended_reference_executable']=identity(ref)
    extended_path=a.root/'family-match-reference.json';encoded=json.dumps(extended,indent=2)
    if a.verify:assert extended_path.read_text()==encoded
    else:extended_path.write_text(encoded)
    rows=[];differences=[]
    for entry in files.values():
        assert identity(entry['font']['path'])==entry['font'];process=subprocess.run([str(exe),entry['font']['path'],str(entry['index'])],capture_output=True,text=True);assert process.returncode==0,process.stdout
        actual=json.loads(process.stdout.splitlines()[0]);row=dict(**entry,native=actual);rows.append(row)
        props=[r['properties'] for r in actual['candidates']]
        if actual['status'] or actual['family']!=entry['family'] or sorted(props)!=sorted(entry['properties']):differences.append(row)
    result=dict(executable=identity(exe),reference=identity(a.reference),arial_reference_executable=identity(ref),arial_reference=observed,faces=len(rows),exact=len(rows)-len(differences),differences=differences,rows=rows)
    output=a.root/f'families-{a.config.lower()}.json';encoded=json.dumps(result,indent=2)
    if a.verify:assert output.read_text()==encoded
    else:output.write_text(encoded)
    print('faces',len(rows),'differences',len(differences));print([(r['font']['path'],r['family'],r['native']['family'],r['properties'],[n['properties'] for n in r['native']['candidates']]) for r in differences]);assert not differences
if __name__=='__main__':main()
