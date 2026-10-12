"""QA-only Latin normalization/font-availability probes against DirectWrite.

The generated fonts contain no GSUB/GPOS rules. Their unique cmap glyphs expose
preprocessing rather than relying on a real font's composition substitutions.
FontTools is used only to write these disposable first-party fixtures.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess
from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib.tables._g_l_y_f import Glyph

CASES=['e\u0301','\u00e9','A\u030a','\u00c5','\u212b','c\u0327\u0301','c\u0301\u0327',
       '\u00e7\u0301','\u1e09','A\u030a\u0301','\u01fa','I\u0307','\u0130',
       'e\u0341','e\u0300','e\u0340','a\u0344','a\u0308\u0301','\ufb03',
       'e\u0301\u034f','e\u034f\u0301','e\u0301\u0301','e\u0301\u0300',
       'a\u0327\u0301','a\u0301\u0327','a\u0315\u0300','a\u0300\u0315',
       '\u0301','\u0344','\u0301a','e\u200d\u0301','e\u200c\u0301']
COMPOSITES={0xe9,0xc5,0x212b,0xe7,0x1e09,0x1fa,0x130,0xe0,0xe8,0xe1,0xe4,0x1df}
SCALARS=sorted({ord(c) for text in CASES for c in text}|COMPOSITES|{0x25cc,32})
def identity(path):
    path=Path(path)
    return dict(path=str(path),bytes=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest())
def make(path,coverage):
    order=['.notdef']+[f'u{cp:06x}' for cp in SCALARS]
    builder=FontBuilder(1000,isTTF=True);builder.setupGlyphOrder(order)
    builder.setupCharacterMap({cp:f'u{cp:06x}' for cp in coverage})
    builder.setupGlyf({g:Glyph() for g in order});builder.setupHorizontalMetrics({g:(600,0) for g in order})
    builder.setupHorizontalHeader(ascent=800,descent=-200)
    name='MaiCord Latin '+path.stem
    builder.setupNameTable(dict(familyName=name,styleName='Regular',uniqueFontIdentifier=name,fullName=name,psName='MaiCordLatin'+path.stem))
    builder.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
    builder.setupPost();builder.setupMaxp();builder.save(path)
    return [None]+SCALARS
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('root',type=Path);parser.add_argument('--exe',required=True,type=Path);parser.add_argument('--pairs',type=Path);args=parser.parse_args()
    args.root.mkdir(parents=True,exist_ok=True)
    global CASES,COMPOSITES,SCALARS
    if args.pairs:
        pairs=json.loads(args.pairs.read_text())['pairs'];by_composed={z:(x,y) for x,y,z in pairs}
        def expanded(cp):return sum((expanded(v) for v in by_composed[cp]),[]) if cp in by_composed else [cp]
        samples=[]
        for x,y,z in pairs:
            decomposed=expanded(z)
            samples += [[x,y],decomposed,[z],decomposed+[0x301],decomposed+[0x34f]]
            if len(decomposed)>2:samples.append([decomposed[0]]+list(reversed(decomposed[1:])))
        CASES=list(dict.fromkeys(CASES+[''.join(chr(c) for c in s) for s in samples]));COMPOSITES|=set(by_composed)
        SCALARS=sorted({ord(c) for text in CASES for c in text}|COMPOSITES|{0x25cc,32})
    sets={'all':set(SCALARS),'decomposed':set(SCALARS)-COMPOSITES,
          'composites':set(SCALARS)-{0x301,0x300,0x30a,0x327,0x307,0x308},
          'missing_base':set(SCALARS)-{ord('e'),ord('A'),ord('c'),ord('I'),ord('a')},
          'no_circle':set(SCALARS)-{0x25cc}}
    rows=[]
    for name,coverage in sets.items():
        path=(args.root/f'latin-{name}.ttf').resolve();glyphs=make(path,coverage)
        for text in CASES:
            command=[str(args.exe),str(path),','.join(f'{ord(c):x}' for c in text),'default','1098907648','0','0','en-US']
            run=subprocess.run(command,capture_output=True,text=True)
            value=json.loads(run.stdout)
            rows.append(dict(font=identity(path),coverage=sorted(coverage),text=[ord(c) for c in text],exit=run.returncode,result=value,
                mapped=[glyphs[g] for g in value.get('glyphs',[])]))
    data=dict(executable=identity(args.exe),dwrite=identity(Path(rows[0]['result']['dwrite_module'])),rows=rows)
    (args.root/'latin-preprocess-oracle.json').write_text(json.dumps(data,indent=2))
    for row in rows:
        if 'all' in Path(row['font']['path']).stem:print(' '.join(f'{cp:04X}' for cp in row['text']),'->',row['mapped'],row['result'].get('clusters'),row['exit'])
if __name__=='__main__':main()
