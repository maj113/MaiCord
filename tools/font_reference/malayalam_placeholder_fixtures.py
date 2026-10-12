"""Controlled font metadata variants for Malayalam placeholder diagnostics."""
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
    args=parser.parse_args();raw=args.font.read_bytes()
    if hashlib.sha256(raw).hexdigest()!='b372c33057cc1c1d2adb7d56158704ca768e3441196b8f0cbeaee6f6ff34b038':
        raise ValueError('Requires the frozen Nirmala face-zero diagnostic input')
    args.output_dir.mkdir(parents=True,exist_ok=True);rows=[]
    for name in ['no-presentation','dot-ka-swap']:
        with TTFont(io.BytesIO(raw),recalcTimestamp=False) as font:
            if name=='no-presentation':
                table=font['GSUB'].table
                script=next(r.Script for r in table.ScriptList.ScriptRecord if r.ScriptTag=='mlm2')
                for index in script.DefaultLangSys.FeatureIndex:
                    record=table.FeatureList.FeatureRecord[index]
                    if record.FeatureTag in ('pres','abvs','blws','psts','haln','calt','rclt'):
                        record.Feature.LookupListIndex=[];record.Feature.LookupCount=0
            else:
                for cmap in font['cmap'].tables:
                    if cmap.isUnicode():cmap.cmap[0xd4e],cmap.cmap[0xd15]=cmap.cmap[0xd15],cmap.cmap[0xd4e]
            stream=io.BytesIO();font.save(stream);data=stream.getvalue()
        path=args.output_dir/(name+'.ttf')
        if path.exists():
            if path.read_bytes()!=data:raise ValueError('Existing fixture differs')
        else:path.write_bytes(data)
        rows.append(dict(name=name,path=str(path.resolve()),sha256=hashlib.sha256(data).hexdigest()))
    (args.output_dir/'manifest.json').write_text(json.dumps(rows,indent=2))
    print('Verified',len(rows),'font controls')


if __name__=='__main__':main()
