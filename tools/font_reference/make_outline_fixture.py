"""Generate first-party static TrueType geometry for the Windows/native oracle.

fontTools is a development-only font writer. No fontTools code or generated
fixture is shipped with MaiCord. Output exercises composite flags and numerical
cases absent from ordinary profile fonts.
"""
import argparse
from array import array
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent, GlyphCoordinates
from fontTools.ttLib.tables.ttProgram import Program


def simple(points, on):
    g = Glyph()
    g.numberOfContours = 1
    g.coordinates = GlyphCoordinates(points)
    g.endPtsOfContours = [len(points) - 1]
    g.flags = array("B", on)
    g.program = Program()
    g.program.fromBytecode([])
    return g


def component(name, x=0, y=0, transform=None, flags=0, attachment=None):
    c = GlyphComponent()
    c.glyphName = name
    c.flags = flags
    if attachment is None:
        c.x, c.y = x, y
    else:
        c.firstPt, c.secondPt = attachment
    if transform is not None:
        c.transform = transform
    return c


def composite(*components):
    g = Glyph()
    g.numberOfContours = -1
    g.components = list(components)
    return g


def make_font(output, units):
    glyphs = {
        ".notdef": Glyph(),
        "triangle": simple([(2, 3), (1952, 55), (398, -975)], [1, 1, 1]),
        "quadratic": simple([(291, -18), (245, -18), (180, 46), (180, 92)], [1, 0, 0, 1]),
        "offcurve": simple([(2, 0), (5, 7), (3, 0)], [0, 0, 0]),
        "single": simple([(17, 29)], [1]),
    }
    transforms = [
        [[.5, 0], [0, .5]],
        [[-16383 / 16384, 0], [0, 1]],
        [[15912 / 16384, 0], [0, 15912 / 16384]],
        [[11593 / 16384, -11588 / 16384], [11588 / 16384, 11593 / 16384]],
        [[-3 / 16384, -16569 / 16384], [-16569 / 16384, -3 / 16384]],
    ]
    for i, transform in enumerate(transforms):
        for flags, label in [(0, "default"), (4, "round"), (0x800, "scaled"), (0x1000, "unscaled")]:
            glyphs[f"matrix{i}_{label}"] = composite(component("triangle", 43, -48, transform, flags))
    glyphs["attached"] = composite(component("triangle"), component("quadratic", attachment=(1, 0)))
    glyphs["attached_transform"] = composite(component("triangle"), component("quadratic", transform=transforms[3], attachment=(2, 0)))
    glyphs["repeat"] = composite(component("triangle"), component("triangle", 50, 0), component("triangle", 100, 0))
    glyphs["nested"] = composite(component("matrix3_default", -39, 18, transforms[2]))
    glyphs["nested_attachment"] = composite(component("nested"), component("attached", attachment=(2, 3)))
    glyphs["use_metrics"] = composite(component("triangle", 19, 0, flags=0x200))
    order = list(glyphs)
    builder = FontBuilder(units, isTTF=True)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap({0xE000 + i: name for i, name in enumerate(order[1:])})
    builder.setupGlyf(glyphs)
    builder.setupHorizontalMetrics({name: (2100, 13 if name != ".notdef" else 0) for name in order})
    builder.setupHorizontalHeader(ascent=2100, descent=-1100)
    builder.setupNameTable({"familyName": "MaiCord Outline Fixture", "styleName": "Regular",
                           "uniqueFontIdentifier": f"MaiCordOutlineFixture{units}",
                           "fullName": f"MaiCord Outline Fixture {units}",
                           "psName": f"MaiCordOutlineFixture{units}"})
    builder.setupOS2(sTypoAscender=2100, sTypoDescender=-1100, usWinAscent=2100, usWinDescent=1100)
    builder.setupPost()
    builder.setupMaxp()
    builder.font["head"].flags &= ~2
    builder.font["head"].created = builder.font["head"].modified = 2082844800
    builder.save(output)
    print(f"{output}: {len(order)} glyphs, {units} units/em")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for units in (1000, 2048):
        make_font(args.output / f"outline-{units}.ttf", units)


if __name__ == "__main__":
    main()
