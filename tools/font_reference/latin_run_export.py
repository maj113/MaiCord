"""Export captured independent Windows observations for the native-only runner."""
from pathlib import Path
import argparse,json,struct
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('input',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    d=json.loads(a.input.read_text());records=[]
    for row in d['rows']:
        r=row['result']
        if row['exit'] or r['hresult'] or r['placement_hresult']:continue
        text=''.join(chr(cp) for cp in row['text']).encode();path=row['font']['path'].encode('utf-16le')
        layout='layout_glyphs' in r;glyphs=r['layout_glyphs'] if layout else r['glyphs'];clusters=r['layout_clusters'] if layout else r['clusters'];placements=r['layout_placement_bits'] if layout else r['placement_bits']
        record=struct.pack('<I',len(path)//2)+path+struct.pack('<I',len(text))+text
        record+=struct.pack('<5I',r['em_bits'],row.get('independent',0),row.get('weight',0),len(glyphs),len(clusters))
        record+=struct.pack('<'+'H'*len(glyphs),*glyphs)+struct.pack('<'+'I'*len(clusters),*clusters)
        record+=b''.join(struct.pack('<3I',*v) for v in placements);records.append(record)
    a.output.write_bytes(b'MCLATIN1'+struct.pack('<I',len(records))+b''.join(records));print(len(records),'Latin reference records')
if __name__=='__main__':main()
