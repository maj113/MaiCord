"""QA-only font: every ASCII A expands to four glyphs, forcing scratch retry."""
from pathlib import Path
import sys
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString

def build(path):
    names = {cp: f'u{cp:04X}' for cp in range(32, 127)}
    order = ['.notdef', *names.values(), 'expanded']
    font = FontBuilder(1024, isTTF=True)
    font.setupGlyphOrder(order)
    font.setupCharacterMap(names)
    glyphs = {}
    for name in order:
        pen = TTGlyphPen(None)
        if name != names[32]:
            pen.moveTo((32, 0)); pen.lineTo((448, 0))
            pen.lineTo((448, 700)); pen.lineTo((32, 700)); pen.closePath()
        glyphs[name] = pen.glyph()
    font.setupGlyf(glyphs)
    font.setupHorizontalMetrics({name: (512, 32) for name in order})
    font.setupHorizontalHeader(ascent=800, descent=-224)
    font.setupNameTable(dict(familyName='MaiCord Paragraph Expansion QA', styleName='Regular',
        uniqueFontIdentifier='MaiCord-Paragraph-Expansion-QA-1',
        fullName='MaiCord Paragraph Expansion QA', psName='MaiCordParagraphExpansionQA'))
    font.setupOS2(sTypoAscender=800, sTypoDescender=-224, usWinAscent=800, usWinDescent=224)
    font.setupPost(); font.setupMaxp()
    addOpenTypeFeaturesFromString(font.font, '''languagesystem DFLT dflt;
        languagesystem latn dflt;
        feature liga { sub u0041 by expanded expanded expanded expanded; } liga;''')
    font.font['head'].created = font.font['head'].modified = 3800000000
    font.font.recalcTimestamp = False
    font.save(path)

if __name__ == '__main__':
    build(Path(sys.argv[1]))
