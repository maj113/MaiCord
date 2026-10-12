"""Authored Sinhala font-language fallback variants; offline FontTools QA only."""
import argparse,json,random
from pathlib import Path
from fontTools.ttLib import TTFont


def generate(source,folder):
    folder.mkdir(parents=True,exist_ok=True)
    names=['no-gsub','non-sinh','empty-sinh','base-class','zero-advance','zero-and-empty','empty','no-tables','no-sinhala','only-gsub']
    for name in names:
        with TTFont(source,recalcTimestamp=False) as font:
            if name in ('no-gsub','no-tables'):del font['GSUB']
            if name in ('only-gsub','no-tables'):del font['GPOS']
            if name in ('non-sinh','no-sinhala'):font['GSUB'].table.ScriptList.ScriptRecord[0].ScriptTag='latn'
            if name=='no-sinhala':font['GPOS'].table.ScriptList.ScriptRecord[0].ScriptTag='latn'
            if name=='empty-sinh':
                language=font['GSUB'].table.ScriptList.ScriptRecord[0].Script.DefaultLangSys
                language.FeatureIndex=[];language.FeatureCount=0
            if name=='base-class':font['GDEF'].table.GlyphClassDef.classDefs['tagD']=1
            if name in ('zero-advance','zero-and-empty'):font['hmtx'].metrics['tagD']=(0,0)
            if name in ('empty','zero-and-empty'):font['glyf']['tagD'].numberOfContours=0
            font.save(folder/(name+'.ttf'))
    cases=[[0xd9a,0xdd9],[0xddd],[0xd9a,0xddd],[0x34f,0x34f],[0x200d,0x34f]]
    rng=random.Random(93013);alphabet=[0xd9a,0xdbb,0xdba,0xdca,0xdd9,0xddd,0xdcf,0xddf,0x200d,0x200c,0x34f,0x20]
    cases += [[rng.choice(alphabet) for _ in range(rng.randrange(1,32))] for _ in range(256)]
    (folder/'nominal-cases.json').write_text(json.dumps(cases),encoding='utf-8')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output-dir',type=Path,required=True);a=p.parse_args()
    generate(a.source,a.output_dir)
