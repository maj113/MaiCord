from pathlib import Path
import argparse,json,struct,hashlib
from embed_fixture import write_embedded_array
def main():
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    rows=json.loads((a.root/'family-probes.json').read_text())['rows'];data=bytearray(b'MCFFAM1\0'+struct.pack('<I',len(rows)))
    for row in rows:
        font=Path(row['font']['path']).read_bytes();assert hashlib.sha256(font).hexdigest()==row['font']['sha256'];name=row['expected']['family'].encode('utf-16-le')
        data+=struct.pack('<IIHBB',len(font),len(name)//2,*row['expected']['properties'])+font+name
    write_embedded_array(a.out,'','alignas(4) constexpr u8 font_family_fixture[]',data);(a.root/'fixture.bin').write_bytes(data);print(len(rows),'cases',len(data),'bytes')
if __name__=='__main__':main()
