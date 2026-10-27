"""Pack first-party Windows observations for native emoji frontend QA."""
import argparse,collections,hashlib,json,struct
from pathlib import Path
from embed_fixture import write_embedded_array

def pack(rows,embedded):
    groups=collections.OrderedDict()
    for row in rows:
        assert row['exit']==0
        groups.setdefault((row['font']['path'],row.get('weight',0)),[]).append(row)
    result=b'MCEMOJI3'+struct.pack('<I',len(groups));manifest=[]
    for (path,weight),group in groups.items():
        font=Path(path);raw=font.read_bytes();digest=hashlib.sha256(raw).hexdigest()
        for row in group:assert digest==row['font']['sha256']
        data=raw if embedded else str(font).encode('utf-8')
        reject=any(row.get('reject_malformed',False) for row in group)
        assert not reject or all(row.get('reject_malformed',False) for row in group)
        result+=struct.pack('<5I',int(embedded),len(data),0 if reject else len(group),int(reject),weight)+data
        for row in [] if reject else group:
            text=''.join(chr(cp) for cp in row['text']).encode('utf-8');d=row['result'];n=len(d['glyphs']);units=len(d['clusters'])
            result+=struct.pack('<4I',d['em_bits'],len(text),n,units)+text
            result+=struct.pack('<'+'H'*n,*d['glyphs'])
            result+=b''.join(struct.pack('<3I',*p) for p in d['placement_bits'])
            result+=struct.pack('<'+'I'*units,*d['clusters'])
            result+=bytes(v[1]*2+v[3] for v in d['glyph_properties'])+bytes(v[2] for v in d['glyph_properties'])
        manifest.append(dict(font=group[0]['font'],weight=weight,cases=0 if reject else len(group),reject_malformed=reject))
    return result,manifest

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--observations',type=Path,action='append',required=True);parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--header',type=Path);args=parser.parse_args()
    rows=[]
    for path in args.observations:
        for row in json.loads(path.read_text())['rows']:
            if 'text' not in row:row['text']=[0x1f600]
            rows.append(row)
    data,fonts=pack(rows,True);args.binary.parent.mkdir(parents=True,exist_ok=True);args.binary.write_bytes(data)
    if args.header:
        write_embedded_array(args.header,'// First-party synthetic fonts and Windows observations; QA only.\n','constexpr u8 emoji_fixture[]',data)
    args.binary.with_suffix('.json').write_text(json.dumps(dict(cases=len(rows),bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),fonts=fonts),indent=2));print(len(rows),'cases',len(fonts),'fonts',len(data),'bytes')

if __name__=='__main__':main()
