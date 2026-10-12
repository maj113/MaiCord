"""First-party nested-GSUB fixtures and installed-Windows observations (offline QA).

fontTools writes test fonts only. It neither computes expected Windows behavior
nor becomes a shipping dependency. Probe results are observations, not native
executor conformance claims. The custom feature references only lookup zero.
"""
import argparse
import copy
import hashlib
import json
import platform
from pathlib import Path
import subprocess
import struct
import sys

import fontTools
from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables import otTables as ot
from fontTools.ttLib.tables._g_l_y_f import Glyph

BASE = list("ABCDEMNXYZL")
ORDER = [".notdef"] + BASE + ["tag" + g for g in BASE]
CMAP = {ord(c): c.upper() for c in "abcdexyzl"}
CMAP.update({0x301: "M", 0x327: "N"})
TEXT = {v: chr(k) for k, v in CMAP.items()}
CLASSES = {g: (3 if g in ("M", "N") else 2 if g == "L" else 1) for g in BASE}
CLASSES.update({"tag" + g: c for g, c in list(CLASSES.items())})


def obj(kind, **fields):
    result = getattr(ot, kind)()
    result.__dict__.update(fields)
    return result


def coverage(glyphs):
    return obj("Coverage", glyphs=list(glyphs))


def lookup(kind, *subtables, flags=0):
    return obj("Lookup", LookupType=kind, LookupFlag=flags,
               SubTableCount=len(subtables), SubTable=list(subtables))


def single(mapping):
    return obj("SingleSubst", mapping=mapping)


def multiple(mapping, flags=0):
    return lookup(2, obj("MultipleSubst", mapping=mapping), flags=flags)


def ligature(inputs, output="L", flags=0):
    return lookup(4, obj("LigatureSubst", ligatures={inputs[0]: [
        obj("Ligature", LigGlyph=output, CompCount=len(inputs), Component=list(inputs[1:]))]}), flags=flags)


def context(inputs, actions, flags=0, chained=False, back=(), ahead=()):
    records = [obj("SubstLookupRecord", SequenceIndex=i, LookupListIndex=k) for i, k in actions]
    if chained:
        table = obj("ChainContextSubst", Format=3, InputGlyphCount=len(inputs),
                    InputCoverage=[coverage([g]) for g in inputs],
                    BacktrackGlyphCount=len(back), BacktrackCoverage=[coverage([g]) for g in back],
                    LookAheadGlyphCount=len(ahead), LookAheadCoverage=[coverage([g]) for g in ahead],
                    SubstCount=len(records), SubstLookupRecord=records)
        kind = 6
    else:
        table = obj("ContextSubst", Format=3, GlyphCount=len(inputs),
                    Coverage=[coverage([g]) for g in inputs], SubstCount=len(records), SubstLookupRecord=records)
        kind = 5
    return lookup(kind, table, flags=flags)


def marker():
    return lookup(1, single({g: "tag" + g for g in BASE}))


def context_format(inputs, actions, fmt, chained=False):
    records = [obj("SubstLookupRecord", SequenceIndex=i, LookupListIndex=k) for i, k in actions]
    if chained:
        rule = obj("ChainSubRule" if fmt == 1 else "ChainSubClassRule", BacktrackGlyphCount=0, Backtrack=[],
                   InputGlyphCount=len(inputs), Input=list(inputs[1:]) if fmt == 1 else [BASE.index(g) + 1 for g in inputs[1:]],
                   LookAheadGlyphCount=0, LookAhead=[], SubstCount=len(records), SubstLookupRecord=records)
        if fmt == 1:
            table = obj("ChainContextSubst", Format=1, Coverage=coverage(inputs[:1]), ChainSubRuleSetCount=1,
                        ChainSubRuleSet=[obj("ChainSubRuleSet", ChainSubRuleCount=1, ChainSubRule=[rule])])
        else:
            sets = [None] * (BASE.index(inputs[0]) + 2)
            sets[-1] = obj("ChainSubClassSet", ChainSubClassRuleCount=1, ChainSubClassRule=[rule])
            table = obj("ChainContextSubst", Format=2, Coverage=coverage(inputs[:1]),
                        BacktrackClassDef=obj("ClassDef", classDefs={}), LookAheadClassDef=obj("ClassDef", classDefs={}),
                        InputClassDef=obj("ClassDef", classDefs={g: i + 1 for i, g in enumerate(BASE)}),
                        ChainSubClassSetCount=len(sets), ChainSubClassSet=sets)
        return lookup(6, table)
    rule = obj("SubRule" if fmt == 1 else "SubClassRule", GlyphCount=len(inputs), SubstCount=len(records), SubstLookupRecord=records)
    if fmt == 1:
        rule.Input = list(inputs[1:])
        table = obj("ContextSubst", Format=1, Coverage=coverage(inputs[:1]), SubRuleSetCount=1,
                    SubRuleSet=[obj("SubRuleSet", SubRuleCount=1, SubRule=[rule])])
    else:
        rule.Class = [BASE.index(g) + 1 for g in inputs[1:]]
        sets = [None] * (BASE.index(inputs[0]) + 2)
        sets[-1] = obj("SubClassSet", SubClassRuleCount=1, SubClassRule=[rule])
        table = obj("ContextSubst", Format=2, Coverage=coverage(inputs[:1]),
                    ClassDef=obj("ClassDef", classDefs={g: i + 1 for i, g in enumerate(BASE)}),
                    SubClassSetCount=len(sets), SubClassSet=sets)
    return lookup(5, table)


def extension(source):
    return lookup(7, *[obj("ExtensionSubst", Format=1, ExtensionLookupType=source.LookupType, ExtSubTable=copy.deepcopy(sub))
                       for sub in source.SubTable], flags=source.LookupFlag)


def action_records(sub):
    if sub.Format == 3:
        return sub.SubstLookupRecord
    for sets_name, rules_name in [("SubRuleSet", "SubRule"), ("SubClassSet", "SubClassRule"),
                                   ("ChainSubRuleSet", "ChainSubRule"), ("ChainSubClassSet", "ChainSubClassRule")]:
        if hasattr(sub, sets_name):
            return [r for group in getattr(sub, sets_name) if group for rule in getattr(group, rules_name) for r in rule.SubstLookupRecord]
    raise AssertionError(sub)


def scenarios():
    cases = []

    def add(name, inputs, lookups, purpose, expected=None):
        cases.append(dict(name=name, input_glyphs=list(inputs), text="".join(TEXT[g] for g in inputs),
                          lookups=lookups, purpose=purpose, expected_enabled=expected))

    add("control-single", "ABCD", [marker()], "Prove custom feature activation", ["tag" + g for g in "ABCD"])
    add("control-multiple", "ABCD", [multiple({"A": ["X", "Y"]})], "Prove expansion lookup activation", list("XYBCD"))
    add("control-ligature", "ABCD", [ligature("AB")], "Prove contraction lookup activation", list("LCD"))
    add("control-mark-ignore", "AMBCD", [ligature("AB", flags=8)], "Prove GDEF mark filtering", list("LMCD"))
    for chained in (False, True):
        label = "chain" if chained else "context"
        for size in (2, 3):
            for index in range(4):
                add(f"{label}-expand{size}-target{index}", "ABCD",
                    [context("ABCD", [(0, 1), (index, 2)], chained=chained),
                     multiple({"A": list("XYZ"[:size])}), marker()],
                    "Locate the second action after one-to-many substitution")
        for size in (2, 3):
            for index in range(4):
                add(f"{label}-merge{size}-target{index}", "ABCD",
                    [context("ABCD", [(0, 1), (index, 2)], chained=chained), ligature("ABCD"[:size]), marker()],
                    "Locate the second action after many-to-one substitution")
    for index in range(4):
        add(f"parent-skip-mark-consumed-target{index}", "AMBCD",
            [context("ABCD", [(0, 1), (index, 2)], flags=8), ligature("AMB"), marker()],
            "Nested ligature consumes a mark omitted from the parent matched positions")
        add(f"parent-skip-mark-preserved-target{index}", "AMBCD",
            [context("ABCD", [(0, 1), (index, 2)], flags=8), ligature("AB", flags=8), marker()],
            "Parent and nested lookup both skip the preserved mark")
        add(f"parent-skip-mark-expand-target{index}", "AMBCD",
            [context("ABCD", [(0, 1), (index, 2)], flags=8), multiple({"A": ["X", "Y"]}), marker()],
            "Insertion before a mark omitted from the parent matched positions")
    for index in range(5):
        add(f"parent-include-mark-preserved-target{index}", "AMBCD",
            [context("AMBCD", [(0, 1), (index, 2)]), ligature("AB", flags=8), marker()],
            "Nested ligature preserves a mark included in the parent matched positions")
    add("identity-stops-subtables", "AB", [lookup(1, single({"A": "A"}), single({"A": "X"}))],
        "An identity match precedes another matching subtable", list("AB"))
    first = context("AB", [])
    second = context("AB", [(0, 1)])
    first.SubTable += second.SubTable
    first.SubTableCount = len(first.SubTable)
    add("empty-action-stops-subtables", "AB", [first, marker()],
        "A matching zero-action context precedes another matching subtable", list("AB"))
    add("same-index-actions", "ABCD", [context("ABCD", [(0, 1), (0, 2)]),
        lookup(1, single({"A": "X"})), lookup(1, single({"X": "Y"}))],
        "Repeated sequence index sees the previous replacement", list("YBCD"))
    add("descending-actions", "ABCD", [context("ABCD", [(2, 1), (0, 1)]), marker()],
        "Action order does not have to be monotonic", ["tagA", "B", "tagC", "D"])
    for index in range(4):
        add(f"nested-expand-target{index}", "ABCD", [context("ABCD", [(0, 1), (index, 3)]),
            context("AB", [(0, 2)]), multiple({"A": ["X", "Y"]}), marker()],
            "Rebase the outer frame after a nested context expands its input")
        add(f"nested-merge-target{index}", "ABCD", [context("ABCD", [(0, 1), (index, 3)]),
            context("AB", [(0, 2)]), ligature("AB"), marker()],
            "Rebase the outer frame after a nested context contracts its input")
    add("nested-exceeds-parent-input", "ABCD", [context("A", [(0, 1)]), ligature("AB")],
        "A nested ligature reaches beyond the parent's input sequence")
    add("nested-exceeds-parent-lookahead", "ABCD", [context("A", [(0, 1)], chained=True, ahead="B"), ligature("ABC")],
        "A nested ligature reaches beyond the parent's lookahead")
    add("root-expansion-resume", "AA", [context("A", [(0, 1)]), multiple({"A": ["A", "X"]})],
        "Root scan must advance past its own replacement", list("AXAX"))
    add("root-contraction-resume", "ABAB", [context("AB", [(0, 1)]), ligature("AB")],
        "Root scan resumes at the following original context", list("LL"))
    for index in range(4):
        for replacement in (["M"], ["X", "M"], ["M", "Y"]):
            label = "".join(replacement)
            add(f"new-mark-{label}-target{index}", "ABCD",
                [context("ABCD", [(0, 1), (index, 2)], flags=8), multiple({"A": replacement}), marker()],
                "Distinguish retained matched positions from refiltering newly produced marks")
        add(f"single-new-mark-target{index}", "ABCD",
            [context("ABCD", [(0, 1), (index, 2)], flags=8), lookup(1, single({"A": "M"})), marker()],
            "Check index resolution after a single substitution changes the GDEF class")
    ignore_marker = marker()
    ignore_marker.LookupFlag = 8
    add("single-new-mark-nested-filter", "AB", [context("AB", [(0, 1), (0, 2)]),
        lookup(1, single({"A": "M"})), ignore_marker], "Check effective class used by the nested lookup")
    add("multiple-new-mark-nested-filter", "AB", [context("AB", [(0, 1), (0, 2)]),
        multiple({"A": ["M"]}), ignore_marker], "Compare class handling for single and multiple replacement")
    add("mark-to-base-nested-filter", "AMB", [context("AMB", [(1, 1), (1, 2)]),
        lookup(1, single({"M": "X"})), ignore_marker], "Check a mark becoming a base before a nested filtered lookup")
    add("nested-input-outside-parent", "ABCD", [context("AB", [(0, 1)]),
        context("ABC", [(0, 2)]), marker()], "Nested context requires input outside the parent input")
    add("nested-lookahead-outside-parent", "ABCD", [context("AB", [(0, 1)]),
        context("AB", [(0, 2)], chained=True, ahead="C"), marker()],
        "Nested lookahead examines text beyond the parent input")
    add("nested-backtrack-outside-parent", "ABCD", [context("BC", [(0, 1)]),
        context("BC", [(0, 2)], chained=True, back="A"), marker()],
        "Nested backtrack examines text before the parent input")
    add("nested-consuming-parent-lookahead", "ABCD", [context("AB", [(1, 1)], chained=True, ahead="C"),
        ligature("BC")], "Parent lookahead does not necessarily enlarge the nested input bound")
    add("nested-consuming-trailing-mark", "ABMC", [context("AB", [(1, 1)], flags=8),
        ligature("BM")], "Nested lookup tries to consume a mark after the final parent input")
    add("nested-consuming-intervening-mark", "AMBC", [context("AB", [(0, 1)], flags=8),
        ligature("AM")], "Nested lookup consumes an intervening mark within the parent input bound")
    add("nested-consuming-trailing-mark-unfiltered", "ABMC", [context("AB", [(1, 1)]),
        ligature("BM")], "Compare the trailing-mark bound when the parent does not skip marks")
    add("nested-input-trailing-mark", "ABMC", [context("AB", [(1, 1)], flags=8),
        context("BM", [(1, 2)]), marker()], "Nested context accesses a trailing parent-ignored mark")
    add("nested-trailing-mark-and-next-base", "ABMC", [context("AB", [(1, 1)], flags=8),
        ligature("BMC")], "Trailing ignored marks do not permit consuming the next base")
    add("new-mark-as-later-component", "ABC", [context("ABC", [(1, 1), (0, 2)]),
        lookup(1, single({"B": "M"})), ligature("AC", flags=8)],
        "Determine the effective class of a substituted later component")
    add("new-base-as-later-component", "AMBC", [context("AMB", [(1, 1), (0, 2)]),
        lookup(1, single({"M": "X"})), ligature("AB", flags=8)],
        "Determine whether an old mark class survives substitution to a base")
    add("new-multiple-mark-as-later-component", "ABC", [context("ABC", [(1, 1), (0, 2)]),
        multiple({"B": ["M"]}), ligature("AC", flags=8)],
        "Compare class propagation for a multiple substitution")
    for fmt in (1, 2):
        for chained in (False, True):
            for operation, nested in [("expand", multiple({"A": ["X", "Y"]})), ("merge", ligature("AB"))]:
                for index in (1, 2):
                    add(f"format{fmt}-{'chain' if chained else 'context'}-{operation}-target{index}", "ABCD",
                        [context_format("ABCD", [(0, 1), (index, 2)], fmt, chained), nested, marker()],
                        "Execute an equivalent rule in the other contextual encodings")
    add("extension-single", "ABCD", [extension(marker())], "Execute an extension-wrapped basic lookup")
    add("extension-context-and-nested", "ABCD", [extension(context("ABCD", [(0, 1), (1, 2)])),
        extension(multiple({"A": ["X", "Y"]})), extension(marker())], "Resolve wrappers at both execution levels")
    add("alternate-first", "ABCD", [lookup(3, obj("AlternateSubst", alternates={"A": ["X", "Y", "Z"]}))],
        "Select the first alternate with feature parameter one", list("XBCD"))
    reverse = lookup(8, obj("ReverseChainSingleSubst", Format=1, Coverage=coverage("B"),
        BacktrackGlyphCount=0, BacktrackCoverage=[], LookAheadGlyphCount=1, LookAheadCoverage=[coverage("B")],
        GlyphCount=1, Substitute=["X"]))
    add("reverse-observes-later-edits", "ABBB", [reverse], "Distinguish reverse lookup traversal from forward traversal")
    add("extension-reverse", "ABBB", [extension(reverse)], "Preserve reverse traversal through extension lookup")
    for operation, nested in [("expand", multiple({"M": ["X", "Y"]})), ("merge", ligature("MB")),
                               ("single", lookup(1, single({"M": "X"})))]:
        for index in (1, 2):
            add(f"ignored-origin-{operation}-target{index}", "AMBC",
                [context("ABC", [(0, 1), (index, 3)], flags=8), context("AMBC", [(1, 2)]), nested, marker()],
                "Rebase an ancestor when the nested edit starts outside its matched membership")
    add("base-ligature-effective-class", "EABC", [context("EABC", [(1, 1), (0, 2)]),
        ligature("AB"), ligature("EC", output="X", flags=4)],
        "Check whether a newly formed base ligature is skipped by IgnoreLigatures")
    add("mark-ligature-effective-class", "AMMB", [context("AMMB", [(1, 1), (0, 2)]),
        ligature("MM"), ligature("AB", output="X", flags=8)],
        "Check the effective class of a ligature formed entirely from marks")
    return cases


def make_font(path, case):
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(ORDER)
    builder.setupCharacterMap(CMAP)
    builder.setupGlyf({g: Glyph() for g in ORDER})
    builder.setupHorizontalMetrics({g: (0 if CLASSES.get(g) == 3 else 600, 0) for g in ORDER})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    family = "MaiCord GSUB " + case["name"]
    builder.setupNameTable(dict(familyName=family, styleName="Regular", uniqueFontIdentifier=family,
                               fullName=family, psName="MaiCordGSUB-" + case["name"]))
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    builder.setupPost()
    builder.setupMaxp()
    scripts = []
    for tag in ("DFLT", "latn"):
        lang = obj("LangSys", LookupOrder=None, ReqFeatureIndex=0xffff, FeatureCount=1, FeatureIndex=[0])
        scripts.append(obj("ScriptRecord", ScriptTag=tag,
                           Script=obj("Script", DefaultLangSys=lang, LangSysCount=0, LangSysRecord=[])))
    feature = obj("FeatureRecord", FeatureTag="test",
                  Feature=obj("Feature", FeatureParams=None, LookupCount=1, LookupListIndex=[0]))
    builder.font["GSUB"] = newTable("GSUB")
    builder.font["GSUB"].table = obj("GSUB", Version=0x10000,
        ScriptList=obj("ScriptList", ScriptCount=len(scripts), ScriptRecord=scripts),
        FeatureList=obj("FeatureList", FeatureCount=1, FeatureRecord=[feature]),
        LookupList=obj("LookupList", LookupCount=len(case["lookups"]), Lookup=case["lookups"]))
    builder.font["GDEF"] = newTable("GDEF")
    builder.font["GDEF"].table = obj("GDEF", Version=0x10000,
        GlyphClassDef=obj("ClassDef", classDefs=CLASSES.copy()), AttachList=None,
        LigCaretList=None, MarkAttachClassDef=None)
    builder.font["head"].created = builder.font["head"].modified = 2082844800
    builder.save(path)
    # Reopen the actual compiled bytes and verify top-level scheduling isolation.
    with TTFont(path) as font:
        assert font.getGlyphOrder() == ORDER
        assert font.getBestCmap() == CMAP
        assert font["GDEF"].table.GlyphClassDef.classDefs == CLASSES
        gsub = font["GSUB"].table
        assert gsub.FeatureList.FeatureRecord[0].FeatureTag == "test"
        assert gsub.FeatureList.FeatureRecord[0].Feature.LookupListIndex == [0]
        assert gsub.LookupList.LookupCount == len(case["lookups"])
        for actual, source in zip(gsub.LookupList.Lookup, case["lookups"]):
            assert actual.LookupType == source.LookupType and actual.LookupFlag == source.LookupFlag
            assert actual.SubTableCount == source.SubTableCount
            for sub, original in zip(actual.SubTable, source.SubTable):
                if source.LookupType in (5, 6):
                    assert [(x.SequenceIndex, x.LookupListIndex) for x in action_records(sub)] == [
                        (x.SequenceIndex, x.LookupListIndex) for x in action_records(original)]
        # Decoded QA description aids inspection; it never supplies shaping outputs.
        font.saveXML(path.with_suffix(".ttx"), tables=["GlyphOrder", "cmap", "GDEF", "GSUB"])


def identity(path):
    return dict(path=str(path.resolve()), bytes=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def generate(folder):
    folder.mkdir(parents=True, exist_ok=True)
    records = []
    for case in scenarios():
        path = folder / (case["name"] + ".ttf")
        make_font(path, case)
        records.append({k: v for k, v in case.items() if k != "lookups"} | {"font": identity(path), "decoded": identity(path.with_suffix(".ttx"))})
    result = dict(scope="First-party fixture fonts; Windows outputs not predicted", fonttools=fontTools.__version__,
                  python=sys.version, glyph_order=ORDER, cases=records)
    (folder / "fixtures.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {len(records)} fixtures")


def record(folder, executable, output):
    manifest = json.loads((folder / "fixtures.json").read_text(encoding="utf-8"))
    observations = []
    module = None
    for case in manifest["cases"]:
        path = folder / (case["name"] + ".ttf")
        assert identity(path)["sha256"] == case["font"]["sha256"]
        outputs = []
        for enabled in (0, 1):
            command = [str(executable.resolve()), str(path.resolve()), ",".join(f"{ord(c):x}" for c in case["text"]), str(enabled)]
            run = subprocess.run(command, capture_output=True, text=True, timeout=15)
            if run.returncode:
                raise RuntimeError(f"{case['name']} feature={enabled}, exit={run.returncode}: {run.stdout} {run.stderr}")
            value = json.loads(run.stdout)
            assert value["hresult"] == 0 and value["feature_enabled"] == enabled
            loaded = Path(value["dwrite_module"]).resolve()
            if module is None:
                module = loaded
            assert loaded == module
            value["glyph_names"] = [manifest["glyph_order"][gid] for gid in value["glyphs"]]
            assert len(value["clusters"]) == len(case["text"].encode("utf-16-le")) // 2
            assert all(0 <= i < len(value["glyphs"]) for i in value["clusters"])
            assert len(value["glyph_properties"]) == len(value["glyphs"])
            if not enabled:
                assert value["glyph_names"] == case["input_glyphs"], (case["name"], value)
            elif case["expected_enabled"] is not None:
                assert value["glyph_names"] == case["expected_enabled"], (case["name"], value)
            outputs.append(value)
        observations.append(dict(name=case["name"], disabled=outputs[0], enabled=outputs[1]))
    result = dict(scope="Installed DirectWrite GetGlyphs observations; single Latin run, LTR, en-US, custom test feature",
                  os=platform.platform(), windows_version=list(sys.getwindowsversion()),
                  executable=identity(executable), fixtures=identity(folder / "fixtures.json"),
                  dwrite=identity(module),
                  glyph_property_fields=["justification", "isClusterStart", "isDiacritic", "isZeroWidthSpace"],
                  observations=observations)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(observations)} enabled probes and {len(observations)} disabled controls")


def export(folder, observations_path, output, header, clusters=False):
    manifest = json.loads((folder / "fixtures.json").read_text(encoding="utf-8"))
    observed = json.loads(observations_path.read_text(encoding="utf-8"))
    assert observed["fixtures"]["sha256"] == identity(folder / "fixtures.json")["sha256"]
    records = {x["name"]: x for x in observed["observations"]}
    data = bytearray(b"MCGCL01\0" if clusters else b"MCGEX02\0") + struct.pack("<I", len(manifest["cases"]))
    differences = []
    for case in manifest["cases"]:
        path = folder / (case["name"] + ".ttf")
        assert identity(path)["sha256"] == case["font"]["sha256"]
        raw = path.read_bytes()
        with TTFont(path) as font:
            order = font.getGlyphOrder()
            def original(tag):
                entry = font.reader.tables[tag]
                return raw[entry.offset:entry.offset + entry.length]
            gsub, gdef = original("GSUB"), original("GDEF")
            classes = font["GDEF"].table.GlyphClassDef.classDefs
            row = records[case["name"]]
            glyphs, windows = row["disabled"]["glyphs"], row["enabled"]["glyphs"]
            expected, kind = windows, 0
            if case["name"] == "extension-reverse":
                # The generator wraps a deep copy of the direct reverse lookup.
                # OpenType requires extension equivalence; retain both outputs.
                expected, kind = records["reverse-observes-later-edits"]["enabled"]["glyphs"], 1
                assert expected != windows
                differences.append(dict(case=case["name"], native_reference="reverse-observes-later-edits",
                    reason="OpenType GSUB type 7 requires execution as the referenced lookup type",
                    native=expected, windows=windows))
            assert [order[i] for i in glyphs] == case["input_glyphs"]
            data += struct.pack("<11I", len(order), len(gsub), len(gdef), len(glyphs), 0, len(glyphs), 0, 0, len(expected), kind, len(windows))
            if clusters:
                initial = row["disabled"]["clusters"]
                boundaries = [i for i, value in enumerate(initial) if not i or value != initial[i-1]]
                assert len(glyphs) == len(case["text"])
                sources, at = [], 0
                for character in case["text"]:
                    sources.append(at)
                    at += len(character.encode("utf-16-le")) // 2
                assert at == len(initial)
                data += struct.pack("<2I", len(initial), len(boundaries))
            data += gsub + gdef
            for i, gid in enumerate(glyphs):
                data += struct.pack("<HBB", gid, classes.get(order[gid], 0), 0)
                if clusters:
                    data += struct.pack("<I", sources[i])
            data += struct.pack("<" + "H" * len(expected), *expected)
            data += struct.pack("<" + "H" * len(windows), *windows)
            if clusters:
                data += struct.pack("<" + "I" * len(boundaries), *boundaries)
                data += struct.pack("<" + "I" * len(initial), *row["enabled"]["clusters"])
                assert len(expected) == len(windows)
                data += bytes(p[1] for p in row["enabled"]["glyph_properties"])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    meta = dict(scope="Native glyph-ID execution expectations from recorded Windows; original GSUB/GDEF bytes",
                corpus=identity(output), fixtures=identity(folder / "fixtures.json"), observations=identity(observations_path),
                cases=[c["name"] for c in manifest["cases"]], explicit_windows_differences=differences)
    if clusters:
        meta["scope"] = "Native cluster updates seeded with Windows disabled-feature boundaries; not native initial grouping"
    output.with_suffix(".json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    if header:
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text("// Generated offline QA fixtures. See gsub_execution_oracle.py export.\n" +
                          ("constexpr u8 cluster_execution_fixture[]{\n" if clusters else "constexpr u8 gsub_execution_fixture[]{\n") +
                          "\n".join("    " + ",".join(f"0x{x:02x}" for x in data[i:i+24]) + "," for i in range(0, len(data), 24)) +
                          "\n};\n", encoding="utf-8")
    print(f"Exported {len(manifest['cases'])} measured execution cases, {len(data)} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["generate", "record", "export", "export-clusters"])
    parser.add_argument("folder", type=Path)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--observations", type=Path)
    parser.add_argument("--header", type=Path)
    args = parser.parse_args()
    if args.mode == "generate":
        generate(args.folder)
    elif args.mode == "record":
        if not args.exe or not args.out:
            parser.error("record requires --exe and --out")
        record(args.folder, args.exe, args.out)
    else:
        if not args.observations or not args.out:
            parser.error("export requires --observations and --out")
        export(args.folder, args.observations, args.out, args.header, clusters=args.mode == "export-clusters")


if __name__ == "__main__":
    main()
