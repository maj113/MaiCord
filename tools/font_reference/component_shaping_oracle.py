"""Export first-party scalar-to-GSUB-component-to-GPOS checks; QA only."""
import argparse,json,struct,hashlib
from pathlib import Path
from fontTools.ttLib import TTFont
from embed_fixture import write_embedded_array
from gpos_execution_oracle import identity

def export(folder,observed,out,header):
    manifest=json.loads((folder/'fixtures.json').read_text());windows=json.loads(observed.read_text())
    assert windows['fixtures']['sha256']==identity(folder/'fixtures.json')['sha256']
    by_name={}
    for o in windows['observations']:by_name.setdefault(o['name'],[]).append(o)
    word=lambda *v:struct.pack('<'+'I'*len(v),*v)
    blobs=[];selected=[]
    for case in manifest['cases']:
        if case.get('invalid_input'):continue
        path=folder/(case['name']+'.ttf');assert identity(path)['sha256']==case['font']['sha256']
        with TTFont(path) as font:
            types={s.ExtensionLookupType if l.LookupType==9 else l.LookupType for l in font['GPOS'].table.LookupList.Lookup for s in l.SubTable}
            if not types <= {1,2,3,4,5,6,7,8}:continue
        configs=by_name[case['name']];seed=configs[0]['disabled'];n=len(seed['glyphs']);units=len(case['text'].encode('utf-16-le'))//2
        flags=[int(p[2] or p[3])|int(p[1])*2 for p in seed['glyph_properties']]
        data=word(path.stat().st_size,len(case['text']),units,n,len(configs))+path.read_bytes()
        data+=word(*map(ord,case['text']))+word(*seed['clusters'])
        data+=b''.join(struct.pack('<HBB',g,flag,0) for g,flag in zip(seed['glyphs'],flags))
        for o in configs:
            for state in ['disabled','enabled']:
                assert o[state]['glyphs']==seed['glyphs'] and o[state]['clusters']==seed['clusters']
                assert [int(p[2] or p[3])|int(p[1])*2 for p in o[state]['glyph_properties']]==flags
            data+=word(o['em_bits'],o['rtl'])
            for state in ['disabled','enabled']:data+=b''.join(word(*row) for row in o[state]['placement_bits'])
        blobs.append(data);selected.append(dict(name=case['name'],input_scalars=len(case['text']),output_glyphs=n,text_units=units,configurations=len(configs),font_sha256=case['font']['sha256']))
    raw=b'MCCMP01\0'+word(len(blobs))+b''.join(blobs);out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(raw)
    out.with_suffix('.json').write_text(json.dumps(dict(scope='Native nominal mapping, default-grapheme boundaries, scheduled GSUB, component associations, class/cluster positioning flags and GPOS1-8; no independent script scheduling, general tailoring, complete glyph-property policy or app parity',observations=identity(observed),cases=selected,corpus_sha256=hashlib.sha256(raw).hexdigest()),indent=2)+'\n')
    if header:
        write_embedded_array(header,'// First-party Windows glyph/cluster/placement expectations; no attachment seeds.\n','constexpr u8 component_fixture[]',raw)
    print(f'Exported {len(blobs)} component-shaping fonts, {len(raw)} bytes')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path);p.add_argument('--observations',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--header',type=Path)
    a=p.parse_args();export(a.folder,a.observations,a.out,a.header)
