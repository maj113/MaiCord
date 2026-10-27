"""Export observed first matches, keeping candidate identity and simulations."""
from pathlib import Path
import argparse,json,struct,hashlib
from embed_fixture import write_embedded_array
def main():
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    capture=json.loads((a.root/'capture.json').read_text());raw=[Path(g['raw']['path']) for g in capture['groups']]+sorted((a.root/'synthetic').glob('*.jsonl'))
    data=bytearray(b'MCFMAT1\0'+struct.pack('<I',len(raw)));groups=[];total=0
    for path in raw:
        rows=[json.loads(l) for l in path.read_text().splitlines()];faces={r['id']:r for r in rows if r['kind']=='face'};family=next(r for r in rows if r['kind']=='family')
        base=[faces[i] for i in family['candidates'] if not faces[i]['simulations']];queries=[r for r in rows if r['kind']=='query']
        data+=struct.pack('<II',len(base),len(queries))
        for f in base:data+=struct.pack('<HBB',*f['properties'])
        for q in queries:
            expected=faces[q['matches'][0]]
            matches=[i for i,f in enumerate(base) if f['path']==expected['path'] and f['index']==expected['index'] and f['axes']==expected['axes']]
            assert len(matches)==1,(family['name'],q,matches)
            data+=struct.pack('<HBBIIHBB',*q['request'],matches[0],expected['simulations'],*expected['properties']);total+=1
        groups.append(dict(name=family['name'],candidates=len(base),queries=len(queries),raw=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    write_embedded_array(a.out,'','alignas(4) constexpr u8 font_matching_fixture[]',data)
    (a.root/'fixture.bin').write_bytes(data);result=dict(groups=groups,cases=total,bytes=len(data),sha256=hashlib.sha256(data).hexdigest());(a.root/'fixture.json').write_text(json.dumps(result,indent=2));print(len(groups),'groups',total,'cases',len(data),'bytes')
if __name__=='__main__':main()
