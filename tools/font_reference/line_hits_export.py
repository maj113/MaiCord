"""Export recorded point/range queries; optional first-party recapture."""
from pathlib import Path
import argparse,json,shutil,struct,subprocess
words=lambda *v:struct.pack('<'+'I'*len(v),*v)

def encode(row):
    o=row['observed'];text=row['text'].encode('ascii');em=struct.unpack('<I',struct.pack('<f',row['size']))[0]
    assert o['hit_hresult']==0 and o['edit_hresult']==0 and len(o['layout_lines'])==1
    assert all(len(rects)<=1 for _,_,rects in o['range_hits'])
    data=b'MCLHIT1\0'+words(len(text),em,int(row.get('independent',False)),len(o['point_hits']),len(o['range_hits']))+text
    for x,y,trailing,inside,rect in o['point_hits']:data+=words(x,y,trailing,inside,*rect)
    for first,length,rects in o['range_hits']:
        data+=words(first,length,len(rects))
        for rect in rects:data+=words(*rect)
    return data

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--fixture-source',type=Path);p.add_argument('--exe',type=Path);a=p.parse_args()
    cases=a.root/'cases';cases.mkdir(exist_ok=True);rows=json.loads((a.root/'observed.json').read_text())
    for index,row in enumerate(rows):(cases/f'{index:04}.bin').write_bytes(encode(row))
    print(len(rows),'actual-font query files')
    if a.fixture_source:
        folder=a.root/'fixtures';folder.mkdir(exist_ok=True);captured=[]
        for old in json.loads((a.fixture_source/'observed.json').read_text()):
            path=folder/Path(old['font']).name
            if not path.exists():shutil.copyfile(old['font'],path)
            em=struct.unpack('<I',struct.pack('<f',old['size']))[0];cps=','.join(f'{ord(c):x}' for c in old['text'])
            result=subprocess.run([str(a.exe),str(path),cps,'default',str(em),'0','0','en-US','edit-layout'],capture_output=True,text=True,check=True)
            captured.append(dict(name=old['name'],font=str(path),text=old['text'],size=old['size'],observed=json.loads(result.stdout)))
        (folder/'observed.json').write_text(json.dumps(captured,indent=2))
        data=b'MCLHFX1\0'+words(len(captured))
        for row in captured:
            font=Path(row['font']).read_bytes();case=encode(row);data+=words(len(font),len(case))+font+case
        (folder/'fixture.bin').write_bytes(data)
        header=Path(__file__).resolve().parents[2]/'tests/generated/line_hits_fixture.inc'
        header.write_text('// Recorded first-party point-to-source and selection queries.\nconstexpr u8 line_hits_fixture[]{\n'+'\n'.join('    '+','.join(f'0x{x:02x}' for x in data[i:i+24])+',' for i in range(0,len(data),24))+'\n};\n')
        print(len(captured),'first-party query cases')

if __name__=='__main__':main()
