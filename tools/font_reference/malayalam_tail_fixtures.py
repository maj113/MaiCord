"""Offline feature-removal controls for Malayalam fallback tail diagnostics."""
import argparse
import hashlib
import io
import json
from pathlib import Path
from fontTools.ttLib import TTFont


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font',type=Path,required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args();source=args.font.read_bytes()
    if hashlib.sha256(source).hexdigest()!='4390aaf85f33535fa1fbb93f1cc4ea327d0e308e066d24b37115f09770ec615e':raise ValueError('Requires frozen no-post Nirmala control')
    tags=('pres','abvs','blws','psts','haln','calt','rclt')
    args.output_dir.mkdir(parents=True,exist_ok=True);rows=[]
    for name in (*tags,'all-presentation'):
        with TTFont(io.BytesIO(source),recalcTimestamp=False) as font:
            table=font['GSUB'].table
            script=next(r.Script for r in table.ScriptList.ScriptRecord if r.ScriptTag=='mlm2')
            for i in script.DefaultLangSys.FeatureIndex:
                record=table.FeatureList.FeatureRecord[i]
                if record.FeatureTag in (tags if name=='all-presentation' else (name,)):
                    record.Feature.LookupListIndex=[];record.Feature.LookupCount=0
            out=io.BytesIO();font.save(out);data=out.getvalue()
        path=args.output_dir/(name+'.ttf')
        if path.exists():
            if path.read_bytes()!=data:raise ValueError('Existing fixture differs')
        else:path.write_bytes(data)
        rows.append(dict(name=name,path=str(path.resolve()),sha256=hashlib.sha256(data).hexdigest()))
    (args.output_dir/'manifest.json').write_text(json.dumps(rows,indent=2))
    print(len(rows),'controlled fonts verified')


if __name__=='__main__':main()
