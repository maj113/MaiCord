"""QA: construct candidates from font files, then match frozen Windows queries."""
from pathlib import Path
from fontTools.ttLib import TTFont
import argparse,hashlib,json,struct,subprocess
def identity(path):
    path=Path(path).resolve();data=path.read_bytes();return dict(path=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
def tag(value):return int.from_bytes(value.to_bytes(4,'little'),'big')
def key(record):return (tuple(record['properties']),tuple(tuple(s) for s in record['settings']))
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--reference',type=Path,required=True);p.add_argument('--config',choices=['Release','Debug'],required=True);p.add_argument('--verify',action='store_true');p.add_argument('--family',action='store_true');a=p.parse_args()
    if a.verify:assert a.root.is_dir()
    else:a.root.mkdir(exist_ok=True,parents=True)
    reference=json.loads(a.reference.read_text());exe=a.root/f"{'MaiCordFontFamilyTests' if a.family else 'MaiCordFontCandidatesTests'}.exe";files={};groups=[]
    def run(args):
        result=subprocess.run([str(exe)]+list(map(str,args)),capture_output=True,text=True);assert result.returncode==0,(args,result.stdout,result.stderr)
        return json.loads(result.stdout.splitlines()[0])
    for group in reference['groups']:
        family_files=[];by_identity={}
        for index in group['base']:
            face=group['faces'][str(index)]
            if face['simulations']:continue
            ident=(face['file']['sha256'],face['index'])
            if ident not in files:
                path=Path(face['file']['path']);assert identity(path)==face['file']
                with TTFont(path,fontNumber=face['index'],lazy=True) as font:
                    axes=[int.from_bytes(ax.axisTag.encode(),'big') for ax in font['fvar'].axes] if 'fvar' in font else []
                actual=run([path,face['index']]);assert actual['status']==0,(path,actual)
                files[ident]=dict(font=face['file'],index=face['index'],axes=axes,native=actual,reference=[])
            entry=files[ident];axis_values={tag(t):v for t,v in face['axes']};expected=dict(properties=face['properties'],settings=[[t,axis_values[t]] for t in entry['axes']])
            if expected not in entry['reference']:entry['reference'].append(expected)
            if ident not in by_identity:by_identity[ident]=len(family_files);family_files.append(entry)
        data=bytearray(b'MCFCAND1'+struct.pack('<II',len(family_files),len(group['queries'])))
        for entry in family_files:
            path=entry['font']['path'].encode('utf-16-le');data+=struct.pack('<II',entry['index'],len(path)//2)+path
        for q in group['queries']:
            expected=group['faces'][str(q['matches'][0])];ident=(expected['file']['sha256'],expected['index']);file_index=by_identity[ident];entry=family_files[file_index];values={tag(t):v for t,v in expected['axes']}
            data+=struct.pack('<HBBIHBBII',*q['request'],file_index,*expected['properties'],expected['simulations'],len(entry['axes']))
            for axis in entry['axes']:data+=struct.pack('<II',axis,values[axis])
        path=a.root/f'family-{len(groups):02d}.bin'
        if path.exists():assert path.read_bytes()==data
        else:assert not a.verify;path.write_bytes(data)
        actual=run(['--family',path]);assert actual['queries']==actual['exact']==len(group['queries'])
        groups.append(dict(family=group['family'],input=identity(path),native=actual))
    rows=list(files.values())
    for entry in rows:assert sorted(map(key,entry['reference']))==sorted(map(key,entry['native']['candidates'])),entry
    result=dict(executable=identity(exe),reference=identity(a.reference),faces=len(rows),static_faces=sum(not e['axes'] for e in rows),variable_faces=sum(bool(e['axes']) for e in rows),candidates=sum(len(e['reference']) for e in rows),queries=sum(g['native']['queries'] for g in groups),groups=groups,files=rows)
    output=a.root/f"{'family-' if a.family else ''}candidates-{a.config.lower()}.json";encoded=json.dumps(result,indent=2)
    if a.verify:assert output.read_text()==encoded
    else:output.write_text(encoded)
    print({k:result[k] for k in ['faces','static_faces','variable_faces','candidates','queries']})
if __name__=='__main__':main()
