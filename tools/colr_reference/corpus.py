"""Generate the complete installed COLRv1 base-glyph corpus (fontTools required)."""
import argparse
from pathlib import Path
from fontTools.ttLib import TTFont
p=argparse.ArgumentParser()
p.add_argument('--font',default='C:/Windows/Fonts/seguiemj.ttf')
p.add_argument('--sizes',default='32')
p.add_argument('--output',default='glyphs.txt')
a=p.parse_args()
f=TTFont(a.font)
ids=sorted({f.getGlyphID(r.BaseGlyph) for r in f['COLR'].table.BaseGlyphList.BaseGlyphPaintRecord})
Path(a.output).write_text(''.join(f'{g} {size}\n' for size in map(int,a.sizes.split(',')) for g in ids))
print(f'{len(ids)} base glyphs; sizes {a.sizes}')
