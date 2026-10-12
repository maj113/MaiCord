"""Reproducible Malayalam classification experiments; offline font metadata only."""
import argparse
import hashlib
import io
import json
from pathlib import Path

VARIANTS={'same-save':None,'mlm2-pstf-empty':('pstf',[]),'mlm2-pstf-noop':('pstf',None),
          'mlm2-pref-empty':('pref',[]),'mlm2-blwf-empty':('blwf',[]),
          'pstf-ya':('pstf',[146]),'pstf-va':('pstf',[147]),'pstf-ra':('pstf',[142]),'pstf-va-ya':('pstf',[147,146])}


def main():
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.tables import otTables
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font',type=Path,required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args();raw=args.font.read_bytes()
    # Root IDs below belong to the frozen selected Nirmala face, never runtime.
    if hashlib.sha256(raw).hexdigest()!='b372c33057cc1c1d2adb7d56158704ca768e3441196b8f0cbeaee6f6ff34b038':
        parser.error('This diagnostic requires the frozen Nirmala face-zero input')
    args.output_dir.mkdir(parents=True,exist_ok=True);rows=[]
    for name,selection in VARIANTS.items():
        with TTFont(io.BytesIO(raw),recalcTimestamp=False) as font:
            table=font['GSUB'].table
            script=next(r.Script for r in table.ScriptList.ScriptRecord if r.ScriptTag=='mlm2')
            if selection:
                tag,roots=selection
                index=next(i for i in script.DefaultLangSys.FeatureIndex if table.FeatureList.FeatureRecord[i].FeatureTag==tag)
                feature=table.FeatureList.FeatureRecord[index].Feature
                if roots is None:
                    lookup=otTables.Lookup();lookup.LookupType=1;lookup.LookupFlag=0
                    subtable=otTables.SingleSubst();subtable.mapping={'A':'A'}
                    lookup.SubTable=[subtable];lookup.SubTableCount=1
                    roots=[len(table.LookupList.Lookup)];table.LookupList.Lookup.append(lookup)
                    table.LookupList.LookupCount=len(table.LookupList.Lookup)
                feature.LookupListIndex=list(roots);feature.LookupCount=len(roots)
            stream=io.BytesIO();font.save(stream);data=stream.getvalue()
        output=args.output_dir/(name+'.ttf')
        if output.exists():
            if output.read_bytes()!=data:raise ValueError('Existing font fixture differs: '+str(output))
        else:output.write_bytes(data)
        rows.append({'variant':name,'path':str(output),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
    manifest=args.output_dir/'form-fixtures.json';manifest.write_text(json.dumps(rows,indent=2)+'\n',encoding='utf-8')
    print('Verified',len(rows),'font fixtures')


if __name__=='__main__':raise SystemExit(main())
