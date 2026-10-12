"""Compare registry/file-built native catalog selections with frozen Windows queries."""
from pathlib import Path
from fontTools.ttLib import TTFont
from fractions import Fraction
from axis_oracle import normalize
import argparse,hashlib,json,ntpath,struct,subprocess
def prepared_expectation(path,index,settings):
    with TTFont(path,fontNumber=index,lazy=True) as font:
        axes=font['fvar'].axes if 'fvar' in font else [];values={t:struct.unpack('<f',struct.pack('<I',v))[0] for t,v in settings};coordinates=[]
        for axis in axes:
            tag=int.from_bytes(axis.axisTag.encode(),'big');segment=font['avar'].segments[axis.axisTag] if 'avar' in font else None
            fixed,_=normalize(axis,Fraction(values[tag]),segment)
            coordinates.append(struct.unpack('<I',struct.pack('<f',fixed/65536))[0])
        instructions=False
        if 'glyf' in font:
            for glyph in font.getGlyphOrder():
                program=getattr(font['glyf'][glyph],'program',None)
                if program is not None and program.getBytecode():instructions=True;break
    return coordinates,instructions
def identity(path):
    path=Path(path).resolve()
    with path.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
    return dict(path=str(path),bytes=path.stat().st_size,sha256=digest)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--reference',type=Path,required=True);p.add_argument('--config',choices=['Release','Debug'],required=True);p.add_argument('--verify',action='store_true');a=p.parse_args()
    reference=json.loads(a.reference.read_text());exe=a.root/f'MaiCordFontCatalogTests.exe';groups=reference['groups'];tasks=[dict(custom=None,groups=[g for g in groups if not g.get('custom')])]
    tasks += [dict(custom=g['custom'],groups=[g]) for g in groups if g.get('custom')];results=[];differences=[];total=0;files={};validated={};axes={};preparation_cache={}
    for ti,task in enumerate(tasks):
        queries=[(g,q) for g in task['groups'] for q in g['queries']];data=bytearray(b'MCFQ'+struct.pack('<I',len(queries)+(1 if ti==0 else 0)))
        for group,q in queries:
            name=group['family'].encode('utf-16-le');data+=struct.pack('<IHBB',len(name)//2,*q['request'])+name
        if ti==0:
            name='Arial Rounded MT Bold'.encode('utf-16-le');data+=struct.pack('<IHBB',len(name)//2,400,5,0)+name
        input=a.root/f'queries-{ti:02d}.bin'
        if a.verify:assert input.read_bytes()==data
        else:input.write_bytes(data)
        command=[str(exe)]+(['--default',task['custom']] if task['custom'] else ['--system'])+[str(input)]
        run=subprocess.run(command,capture_output=True,text=True);assert run.returncode==0,(command,run.stdout[-2000:],run.stderr)
        records=[json.loads(l) for l in run.stdout.splitlines() if l.startswith('{')];raw=a.root/f'catalog-{a.config.lower()}-{ti:02d}.jsonl'
        if a.verify:assert raw.read_text()==run.stdout
        else:raw.write_text(run.stdout)
        observed_files={r['id']:r for r in records if r['kind']=='file'};faces={r['id']:r for r in records if r['kind']=='face'};candidates={r['id']:r for r in records if r['kind']=='candidate'};matches=[r for r in records if r['kind']=='match']
        additions=[r for r in records if r['kind']=='add'];rejected=[r for r in additions if r['error']]
        for addition in additions:
            path=addition['path'];norm=ntpath.normcase(ntpath.normpath(path))
            if norm not in files:files[norm]=identity(path)
        assert len(matches)==len(queries)+(1 if ti==0 else 0)
        exact=0
        for qi,(group,q) in enumerate(queries):
            expected=group['faces'][str(q['matches'][0])];expected_path=expected['file']['path']
            if expected_path not in validated:validated[expected_path]=identity(expected_path)
            assert validated[expected_path]==expected['file'];actual=matches[qi];total+=1
            if actual['status']:
                differences.append(dict(family=group['family'],request=q['request'],actual=actual));continue
            candidate=candidates[actual['candidate']];face=faces[candidate['face']];font=observed_files[face['file']];font=files[ntpath.normcase(ntpath.normpath(font['path']))]
            face_key=(expected['file']['sha256'],expected['index'])
            if face_key not in axes:
                with TTFont(expected_path,fontNumber=expected['index'],lazy=True) as source:axes[face_key]=[int.from_bytes(v.axisTag.encode(),'big') for v in source['fvar'].axes] if 'fvar' in source else []
            expected_axes={int.from_bytes(t.to_bytes(4,'little'),'big'):v for t,v in expected['axes']};expected_settings=[[t,expected_axes[t]] for t in axes[face_key]]
            same=font['sha256']==expected['file']['sha256'] and face['index']==expected['index'] and actual['properties']==expected['properties'] and actual['simulations']==expected['simulations'] and candidate['settings']==expected_settings
            if same:exact+=1
            else:differences.append(dict(family=group['family'],request=q['request'],expected=expected,actual=dict(match=actual,candidate=candidate,face=face,file=font)))
        if ti==0:assert matches[-1]['status']==8,'The absent legacy Arial Rounded name must not become a native alias'
        acquisitions=[r for r in records if r['kind']=='acquire'];assert {r['candidate'] for r in acquisitions}=={r['candidate'] for r in matches if not r['status']}
        assert all(r['released'] for r in acquisitions)
        preparations=[r for r in records if r['kind']=='prepared'];assert len(preparations)==2*len(acquisitions)
        assert {(r['candidate'],r['mode']) for r in preparations}=={(r['candidate'],mode) for r in acquisitions for mode in [0,1]}
        for r in preparations:
            candidate=candidates[r['candidate']];face=faces[candidate['face']];path=observed_files[face['file']]['path'];key=(path,face['index'],tuple(map(tuple,candidate['settings'])))
            if key not in preparation_cache:preparation_cache[key]=prepared_expectation(path,face['index'],candidate['settings'])
            coordinates,instructions=preparation_cache[key];assert r['coordinate_bits']==coordinates,(path,r,coordinates)
            assert r['instructions']==int(bool(r['mode']) and instructions),(path,r,instructions)
        stress=next(r for r in records if r['kind']=='session_capacity');assert stress==dict(kind='session_capacity',retained=min(64,len(acquisitions)),capacity_failures=max(0,len(acquisitions)-64),released=True)
        publication=next(r for r in records if r['kind']=='publication');assert publication['used_bytes']<=publication['committed_bytes'] and publication['committed_bytes']%4096==0
        results.append(dict(custom=task['custom'],input=identity(input),observations=identity(raw),catalog=next(r for r in records if r['kind']=='catalog'),publication=publication,rejected=rejected,queries=len(queries),exact=exact,acquired_candidates=len(acquisitions),prepared_instances=len(preparations),session_capacity=stress))
    output=dict(executable=identity(exe),reference=identity(a.reference),queries=total,exact=sum(r['exact'] for r in results),differences=differences,catalogs=results,files=list(files.values()))
    target=a.root/f'catalog-{a.config.lower()}.json';encoded=json.dumps(output,indent=2)
    if a.verify:assert target.read_text()==encoded
    else:target.write_text(encoded)
    print(dict(catalogs=len(results),queries=total,exact=output['exact'],differences=len(differences),system=results[0]['catalog']))
    if differences:print(json.dumps(differences[:3],indent=2));raise SystemExit(1)
if __name__=='__main__':main()
