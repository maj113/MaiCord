"""Generate a first-party Ethiopic/Cherokee script-selection font (QA only)."""
from pathlib import Path
import re
from fontTools.ttLib import TTFont, newTable
from gsub_execution_oracle import make_font, lookup, single
from feature_plan_oracle import layout, language, script


def generate(output):
    output.parent.mkdir(parents=True, exist_ok=True)
    make_font(output, dict(name='MaiCordExtendedScripts', lookups=[lookup(1, single({'A': 'A'}))]))
    source = (Path(__file__).resolve().parents[2]/'tests/standard_script_cases.inc').read_text(encoding='utf-8')
    strings = re.findall(r'utf8_literal\("([^"\n]*)"\)', source)
    scalars = {ord(c) for text in strings for c in text}
    with TTFont(output, recalcTimestamp=False) as font:
        for table in font['cmap'].tables:
            if not table.isUnicode():
                continue
            for cp in scalars:
                if 0x1200 <= cp <= 0x139f or 0x2d80 <= cp <= 0x2ddf or 0xab00 <= cp <= 0xab2f:
                    table.cmap[cp] = 'A'
                elif 0x13a0 <= cp <= 0x13ff or 0xab70 <= cp <= 0xabbf:
                    table.cmap[cp] = 'B'
        # The requested script must win over both Latin and DFLT. A nominal or
        # incorrectly packed Common profile would produce different glyph IDs.
        features = [('ccmp', [0]), ('ccmp', [1]), ('ccmp', [2]), ('liga', [3])]
        lookups = [lookup(1, single({'A': 'X'})), lookup(1, single({'B': 'Y'})),
                   lookup(1, single({'A': 'Z', 'B': 'Z'})), lookup(1, single({'X': 'tagX', 'Y': 'tagY'}))]
        scripts = {'ethi': script(language([0, 3])), 'cher': script(language([1, 3])),
                   'latn': script(language([2, 3])), 'DFLT': script(language([2, 3]))}
        font['GSUB'] = newTable('GSUB')
        font['GSUB'].table = layout('GSUB', features, lookups, scripts)
        font.save(output)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    generate(parser.parse_args().output)
