"""Offline controlled fonts for Malayalam default-position ordering diagnostics."""
import argparse
import hashlib
import io
import json
from pathlib import Path


def main():
    from fontTools.ttLib import TTFont
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font',type=Path,required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args();source=args.font.read_bytes()
    # The input must be the mlm2-pstf-empty fixture recorded in form-fixtures.json.
    # Feature mutations leave cmap alone except in the named swap controls.
    expected=json.loads((args.font.parent/'form-fixtures.json').read_text(encoding='utf-8'))
    identity=next(r['sha256'] for r in expected if r['variant']=='mlm2-pstf-empty')
    if hashlib.sha256(source).hexdigest()!=identity:raise ValueError('Wrong classification fixture')
    args.output_dir.mkdir(parents=True,exist_ok=True);rows=[]
    for name,remove,swap in [('no-blwf','blwf',None),('no-akhn','akhn',None),('swap-ka-rra',None,(0xd15,0xd31)),('swap-ka-lla',None,(0xd15,0xd33))]:
        font=TTFont(io.BytesIO(source),recalcTimestamp=False);table=font['GSUB'].table
        if remove:
            script=next(r.Script for r in table.ScriptList.ScriptRecord if r.ScriptTag=='mlm2')
            for i in script.DefaultLangSys.FeatureIndex:
                feature=table.FeatureList.FeatureRecord[i]
                if feature.FeatureTag==remove:feature.Feature.LookupListIndex=[];feature.Feature.LookupCount=0
        if swap:
            a,b=swap
            for cmap in font['cmap'].tables:
                if cmap.isUnicode() and a in cmap.cmap and b in cmap.cmap:cmap.cmap[a],cmap.cmap[b]=cmap.cmap[b],cmap.cmap[a]
        stream=io.BytesIO();font.save(stream);raw=stream.getvalue();path=args.output_dir/(name+'.ttf')
        if path.exists() and path.read_bytes()!=raw:raise ValueError('Existing font differs')
        if not path.exists():path.write_bytes(raw)
        rows.append(dict(name=name,path=str(path),sha256=hashlib.sha256(raw).hexdigest()))
    manifest=args.output_dir/'diagnostic-fonts.json';raw=json.dumps(dict(source_sha256=identity,fonts=rows),indent=2).encode('utf-8')
    if manifest.exists() and json.loads(manifest.read_text(encoding='utf-8'))!=json.loads(raw):raise ValueError('Existing manifest differs')
    if not manifest.exists():manifest.write_bytes(raw)
    print(len(rows),'font controls verified')


if __name__=='__main__':main()
