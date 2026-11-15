"""First-party CFF2 test collection; fontTools is only a development font writer.

Two faces in one collection, as Noto Sans CJK ships on NixOS and Fedora (NotoSansCJK-VF.otf.ttc): a CFF2 table with a
variation store of one region, a wght axis from 100 to 900 (default 100), named instances Thin, Regular and Bold, and
STAT for the axis. Face 0 is the family "MaiCord CFF2 Fixture", face 1 "MaiCord CFF2 Fixture B". U+4E00 maps to the
glyph "bar" in both:

  bar     rmoveto (100 + 100 * s, 100), a 300 by 50 box; s is the region scalar (0 at wght 100, 1 at wght 900)
  subrs   the same box drawn through a local and a global subroutine, with an explicit vsindex
  hinted  hstem and hintmask before the box, with blended stem values

Run:

    python tools/font_reference/make_cff2_fixture.py [--header tests/generated/cff2_collection_fixture.inc]
"""
import argparse
import io
from pathlib import Path

from fontTools.cffLib import SubrsIndex
from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString
from fontTools.otlLib.builder import buildStatTable
from fontTools.ttLib import TTCollection, TTFont

from embed_fixture import write_embedded_array

BOX = [300, 0, 0, 50, -300, 0, "rlineto"]


def make_face(family):
    builder = FontBuilder(1000, isTTF=False)
    order = [".notdef", "bar", "subrs", "hinted"]
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap({0x4E00: "bar", 0x4E01: "subrs", 0x4E02: "hinted"})
    programs = {
        ".notdef": [],
        "bar": [100, 100, 1, "blend", 100, "rmoveto"] + BOX,
        "subrs": [0, "vsindex", 100, 100, 1, "blend", 100, "rmoveto", -107, "callsubr", -107, "callgsubr"],
        "hinted": [10, 5, 1, "blend", 20, "hstem", "hintmask", b"\x80", 100, 100, 1, "blend", 100, "rmoveto"] + BOX,
    }
    # fontTools sets up the CFF2 regions from fvar, and fvar its instance names in the name table.
    builder.setupNameTable({"familyName": family, "styleName": "Thin", "uniqueFontIdentifier": family,
                            "fullName": family + " Thin", "psName": family.replace(" ", "") + "-Thin"})
    builder.setupFvar([("wght", 100, 100, 900, "Weight")],
                      [{"location": {"wght": 100}, "stylename": "Thin"},
                       {"location": {"wght": 400}, "stylename": "Regular"},
                       {"location": {"wght": 700}, "stylename": "Bold"}])
    builder.setupCFF2({name: T2CharString(program=program) for name, program in programs.items()},
                      regions=[{"wght": (0.0, 1.0, 1.0)}])
    cff = builder.font["CFF2"].cff
    top = cff.topDictIndex[0]
    private = top.FDArray[0].Private
    private.Subrs = SubrsIndex()
    private.Subrs.append(T2CharString(program=[300, 0, 0, 50, "rlineto"], private=private, globalSubrs=cff.GlobalSubrs))
    cff.GlobalSubrs.append(T2CharString(program=[-300, 0, "rlineto"], private=private, globalSubrs=cff.GlobalSubrs))
    builder.setupHorizontalMetrics({name: (1000, 0) for name in order})
    builder.setupHorizontalHeader(ascent=880, descent=-120)
    builder.setupOS2(sTypoAscender=880, sTypoDescender=-120, usWinAscent=880, usWinDescent=120, usWeightClass=100)
    builder.setupPost()
    buildStatTable(builder.font, [{"tag": "wght", "name": "Weight", "values": [
        {"value": 100, "name": "Thin"}, {"value": 400, "name": "Regular", "flags": 2}, {"value": 700, "name": "Bold"}]}])
    builder.font["head"].created = builder.font["head"].modified = 2082844800
    builder.font.recalcTimestamp = False
    stream = io.BytesIO()
    builder.save(stream)
    stream.seek(0)
    return TTFont(stream)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", type=Path, default=Path("tests/generated/cff2_collection_fixture.inc"))
    args = parser.parse_args()
    collection = TTCollection()
    collection.fonts = [make_face("MaiCord CFF2 Fixture"), make_face("MaiCord CFF2 Fixture B")]
    stream = io.BytesIO()
    collection.save(stream)
    data = stream.getvalue()
    write_embedded_array(args.header, "// First-party CFF2 collection: tools/font_reference/make_cff2_fixture.py.\n",
                         "static constexpr u8 cff2_collection_fixture[]", data)
    print(args.header, len(data), "bytes")


if __name__ == "__main__":
    main()
