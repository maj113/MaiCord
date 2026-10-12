"""Small first-party font for Gurmukhi form eligibility and reordering QA."""
import argparse
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.ttLib.tables._g_l_y_f import Glyph


def make(path, half):
    scalars = [32, *range(0xa00,0xa80),0x200c,0x200d,0x25cc]
    names = {c:f'u{c:04x}' for c in scalars}
    extra = ['halfka','halfya','belowra','belowva','belowha','postya','tailalt']
    order = ['.notdef',*names.values(),*extra]
    font = FontBuilder(1000,isTTF=True)
    font.setupGlyphOrder(order)
    font.setupCharacterMap(names)
    font.setupGlyf({name:Glyph() for name in order})
    font.setupHorizontalMetrics({name:(500+i,0) for i,name in enumerate(order)})
    font.setupHorizontalHeader(ascent=800,descent=-200)
    family = 'MaiCord Gurmukhi Forms '+('Half' if half else 'Full')
    font.setupNameTable(dict(familyName=family,styleName='Regular',fullName=family,
        uniqueFontIdentifier=family,psName=family.replace(' ','')))
    font.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
    font.setupPost()
    font.setupMaxp()
    marks = [0xa01,0xa02,0xa3c,0xa3e,0xa3f,0xa40,0xa41,0xa42,0xa47,0xa48,0xa4b,0xa4c,0xa4d,0xa51,0xa70,0xa71,0xa75]
    mark_names = {names[c] for c in marks}
    mark_names.update(('belowra','belowva','belowha'))
    bases = ' '.join(n for n in order if n not in mark_names)
    features = '''languagesystem gur2 dflt;
        feature blwf { sub u0a4d u0a30 by belowra; sub u0a4d u0a35 by belowva;
                       sub u0a4d u0a39 by belowha; } blwf;
        feature pstf { sub u0a4d u0a2f by postya; sub u0a70 by tailalt; } pstf;
        feature kern { pos u0a15 u0a15 -11; } kern;
    '''
    if half:
        features += 'feature half { sub u0a15 u0a4d by halfka; sub u0a2f u0a4d by halfya; } half;'
    features += f'table GDEF {{ GlyphClassDef [{bases}], , [{" ".join(sorted(mark_names))}], ; }} GDEF;'
    addOpenTypeFeaturesFromString(font.font,features)
    font.font['head'].created = font.font['head'].modified = 2082844800
    font.save(path)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args()
    args.output_dir.mkdir(parents=True,exist_ok=True)
    for half in (False,True):make(args.output_dir/('half.ttf' if half else 'full.ttf'),half)
