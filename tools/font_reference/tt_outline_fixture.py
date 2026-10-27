"""Embed first-party static instruction fonts and complete Windows outlines."""
import argparse,json,struct,subprocess
from pathlib import Path
from embed_fixture import write_embedded_array
from feature_plan_oracle import identity
def run(directories,exe,root,header):
    root.mkdir(parents=True,exist_ok=True);fonts=[p for d in directories for p in sorted(d.glob('*.ttf'))]
    blob=bytearray(b'MCTTGL1\0');blob.extend(struct.pack('<I',len(fonts)));records=[]
    for i,p in enumerate(fonts):
        result=subprocess.run([str(exe.resolve()),str(p.resolve()),'--geometry-dump'],capture_output=True,text=True,timeout=60)
        (root/(str(i)+'-'+p.stem+'.txt')).write_text(result.stdout+result.stderr)
        outlines=[list(map(int,line.split('=')[1].split(','))) for line in result.stdout.splitlines() if line.startswith('gvar_outline=')]
        assert result.returncode in [0,6] and len(outlines)==4
        font=p.read_bytes();blob.extend(struct.pack('<II',len(font),len(outlines)));blob.extend(font)
        for outline in outlines:blob.extend(struct.pack('<'+'I'*len(outline),*outline))
        records.append(dict(font=identity(p),outlines=outlines,reference_returncode=result.returncode))
    write_embedded_array(header,'// First-party static instruction fonts and Windows outline expectations.\n','static constexpr u8 tt_outline_fixture[]',blob)
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),fixture=identity(header),packed_bytes=len(blob),cases=records),indent=2)+'\n')
    print('fonts',len(fonts),'outlines',sum(len(r['outlines']) for r in records),'packed_bytes',len(blob))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--directories',nargs='+',type=Path,required=True);p.add_argument('--exe',type=Path,required=True);p.add_argument('--root',type=Path,required=True);p.add_argument('--header',type=Path,required=True);a=p.parse_args();run(a.directories,a.exe,a.root,a.header)
