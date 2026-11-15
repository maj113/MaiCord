"""First-party variable font with an optical size axis; fontTools is only a development font writer.

The family "MaiCord Opsz Fixture" has the layout of Adwaita Sans, the interface font of GNOME 48: fvar varies opsz
(14 to 32, default 14) and wght (100 to 900, default 400), and STAT names the opsz axis with its own name ID, not the
one fvar gives it. Named instances: Regular (wght 400) and Bold (wght 700), both at opsz 14. The glyphs are
.notdef and "A" (a box), with no variation deltas. tests/ui/text/native_catalog_linux_test.cpp (--desktop-font)
installs it as the desktop interface font. Run:

    python tools/font_reference/make_opsz_fixture.py [--header tests/generated/opsz_fixture.inc]
"""
import argparse
import io
from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.otlLib.builder import buildStatTable
from fontTools.pens.ttGlyphPen import TTGlyphPen

from embed_fixture import write_embedded_array

FAMILY = "MaiCord Opsz Fixture"


def box(left, bottom, right, top):
    pen = TTGlyphPen(None)
    pen.moveTo((left, bottom))
    pen.lineTo((left, top))
    pen.lineTo((right, top))
    pen.lineTo((right, bottom))
    pen.closePath()
    return pen.glyph()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", type=Path, default=Path("tests/generated/opsz_fixture.inc"))
    args = parser.parse_args()
    builder = FontBuilder(1000, isTTF=True)
    order = [".notdef", "A"]
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap({0x41: "A"})
    builder.setupGlyf({".notdef": box(50, 0, 450, 700), "A": box(100, 0, 500, 700)})
    builder.setupHorizontalMetrics({".notdef": (500, 50), "A": (600, 100)})
    builder.setupHorizontalHeader(ascent=900, descent=-200)
    builder.setupNameTable({"familyName": FAMILY, "styleName": "Regular", "uniqueFontIdentifier": FAMILY,
                            "fullName": FAMILY, "psName": FAMILY.replace(" ", "") + "-Regular"})
    builder.setupOS2(sTypoAscender=900, sTypoDescender=-200, usWinAscent=900, usWinDescent=200, usWeightClass=400)
    builder.setupPost()
    builder.setupFvar([("opsz", 14, 14, 32, "Optical size"), ("wght", 100, 400, 900, "Weight")],
                      [{"location": {"opsz": 14, "wght": 400}, "stylename": "Regular"},
                       {"location": {"opsz": 14, "wght": 700}, "stylename": "Bold"}])
    buildStatTable(builder.font, [
        {"tag": "opsz", "name": "Optical Size Axis", "values": [{"value": 14, "name": "Text", "flags": 2}]},
        {"tag": "wght", "name": "Weight", "values": [{"value": 400, "name": "Regular", "flags": 2},
                                                       {"value": 700, "name": "Bold"}]}])
    fvar_names = {axis.axisTag: axis.axisNameID for axis in builder.font["fvar"].axes}
    stat_names = {axis.AxisTag: axis.AxisNameID for axis in builder.font["STAT"].table.DesignAxisRecord.Axis}
    assert stat_names["opsz"] != fvar_names["opsz"], (stat_names, fvar_names)
    builder.font["head"].created = builder.font["head"].modified = 2082844800
    builder.font.recalcTimestamp = False
    stream = io.BytesIO()
    builder.save(stream)
    data = stream.getvalue()
    write_embedded_array(args.header, "// First-party opsz and wght variable font: tools/font_reference/make_opsz_fixture.py.\n",
                         "static constexpr u8 opsz_fixture[]", data)
    print(args.header, len(data), "bytes")


if __name__ == "__main__":
    main()
