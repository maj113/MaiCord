"""QA-only native font metadata comparison against FontTools and app identities."""
from pathlib import Path
import argparse,hashlib,json,struct,subprocess
from fontTools.ttLib import TTFont

def identity(path):
    path=path.resolve();data=path.read_bytes()
    return dict(path=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest())

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path)
    p.add_argument('--config',choices=['Release','Debug'],required=True)
    p.add_argument('--application',type=Path,required=True);p.add_argument('--scope',type=Path,required=True)
    a=p.parse_args();a.root.mkdir(exist_ok=True,parents=True);inputs={}
    def add(path,index,family=None):
        path=Path(path);key=(hashlib.sha256(path.read_bytes()).hexdigest(),index)
        item=inputs.setdefault(key,dict(font=identity(path),index=index,app_families=[]))
        if family and family not in item['app_families']:item['app_families'].append(family)
    for line in a.application.read_text().splitlines():
        if not line.startswith('{'):continue
        row=json.loads(line)
        if row['kind']=='face':add(row['path'],row['index'],row['family'])
    scope=json.loads(a.scope.read_text())
    for item in scope['bundled']+scope['registered_selected_system_fonts']:add(item['file']['path'],0)
    rows=[];records=0;names=0;family_checks=0;differences=[]
    exe=a.root/f'MaiCordFontNamesTests.exe'
    for item in inputs.values():
        path=Path(item['font']['path']);data=path.read_bytes()
        with TTFont(path,fontNumber=item['index'],lazy=True) as f:
            entries=[]
            for r in f['name'].names:
                if (r.platformID==0 and r.platEncID<=6) or (r.platformID==3 and r.platEncID in [0,1,10]):
                    entries.append([r.platformID,r.platEncID,r.langID,r.nameID,r.string.decode('utf-16-be')])
            english=[]
            for id in [1,2,4,6,16,17,21,22]:
                # Required-file audit independently requires the existing US
                # Windows name, when present; synthetic tests cover fallbacks.
                r=f['name'].getName(id,3,1,0x409) or f['name'].getName(id,3,10,0x409)
                english.append([id,r.toUnicode() if r else None])
            os2=f['OS/2'];expected=dict(face_index=item['index'],face_count=struct.unpack_from('>I',data,8)[0] if data[:4]==b'ttcf' else 1,
                attributes=[os2.usWeightClass,os2.usWidthClass,os2.fsSelection,os2.version],records=entries,english=english)
        process=subprocess.run([str(exe),str(path),str(item['index'])],capture_output=True,text=True)
        assert process.returncode==0,(item,process.stdout,process.stderr)
        actual=json.loads(process.stdout.splitlines()[0]);row=dict(**item,expected=expected,native=actual);rows.append(row)
        if expected!=actual:differences.append(row)
        records+=len(entries);names+=sum(v is not None for _,v in english)
        available=dict(actual['english']);family=available[21] or available[16] or available[1]
        for observed in item['app_families']:
            assert family==observed,(path,item['index'],family,observed)
            family_checks+=1
    result=dict(executable=identity(exe),application=identity(a.application),scope=identity(a.scope),faces=len(rows),files=len({r['font']['sha256'] for r in rows}),records=records,
        english_names=names,app_family_checks=family_checks,exact=len(rows)-len(differences),differences=differences,rows=rows)
    (a.root/f'metadata-{a.config.lower()}.json').write_text(json.dumps(result,indent=2))
    print({k:result[k] for k in ['faces','files','records','english_names','app_family_checks','exact']});assert not differences

if __name__=='__main__':main()
