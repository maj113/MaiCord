"""Original GPOS values, anchors and single/pair match expectations (offline QA).

fontTools decodes original font bytes. Only the explicitly labeled fixture is
compiled. This oracle does not predict Windows pixel or placement arithmetic.
"""
import argparse, collections, hashlib, json, struct
from pathlib import Path
import fontTools
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables import otTables as ot
from fontTools.ttLib.tables.otBase import BaseTable, ValueRecordFactory, ValueRecord, valueRecordFormat
from gsub_execution_oracle import obj, coverage, lookup
from embed_fixture import write_embedded_array

FIELDS = ["XPlacement", "YPlacement", "XAdvance", "YAdvance"]
DEVICES = ["XPlaDevice", "YPlaDevice", "XAdvDevice", "YAdvDevice"]
word = lambda *v: struct.pack("<" + "I" * len(v), *v)


def device_blob(d):
    if not d:
        return struct.pack("<H", 0)
    values = d.DeltaValue if d.DeltaFormat != 0x8000 else []
    return struct.pack("<HHHI", d.DeltaFormat, d.StartSize, d.EndSize, len(values)) + struct.pack("<" + "h" * len(values), *values)


def value_blob(v):
    return struct.pack("<4h", *(getattr(v, f, 0) for f in FIELDS)) + b"".join(device_blob(getattr(v, f, None)) for f in DEVICES)


def synthetic():
    font = TTFont(); font.setGlyphOrder(["g0", "g1", "g2", "g3", "g4"])
    devices = [obj("Device", StartSize=15, EndSize=17, DeltaFormat=1, DeltaValue=[-1, 1, -2]),
               obj("Device", StartSize=8, EndSize=10, DeltaFormat=2, DeltaValue=[-8, 7, -1]),
               obj("Device", StartSize=16, EndSize=18, DeltaFormat=3, DeltaValue=[-128, 127, -5]),
               obj("Device", StartSize=0, EndSize=0, DeltaFormat=0x8000)]
    def value(mask, bias=0):
        v = ValueRecord(mask)
        for i, name in enumerate(FIELDS):
            if mask & (1 << i): setattr(v, name, [-30, 120, -255, 32760][i] + bias)
        for i, name in enumerate(DEVICES):
            if mask & (16 << i): setattr(v, name, devices[i])
        return v
    single1 = obj("SinglePos", Format=1, Coverage=coverage(["g1", "g3"]), ValueFormat=255, Value=value(255))
    single2 = obj("SinglePos", Format=2, Coverage=coverage(["g1", "g3"]), ValueFormat=5, ValueCount=2, Value=[value(5), value(5, 1)])
    sets = []
    for glyphs in (["g2", "g4"], ["g2"]):
        pairs = [obj("PairValueRecord", SecondGlyph=g, Value1=value(255, i), Value2=value(15, -i)) for i, g in enumerate(glyphs)]
        sets.append(obj("PairSet", PairValueCount=len(pairs), PairValueRecord=pairs))
    pair1 = obj("PairPos", Format=1, Coverage=coverage(["g1", "g3"]), ValueFormat1=255, ValueFormat2=15, PairSetCount=2, PairSet=sets)
    rows = [obj("Class1Record", Class2Record=[obj("Class2Record", Value1=value(68, i+j), Value2=None) for j in range(3)]) for i in range(3)]
    pair2 = obj("PairPos", Format=2, Coverage=coverage(["g1", "g3"]), ValueFormat1=68, ValueFormat2=0,
                ClassDef1=obj("ClassDef", classDefs={"g1": 1, "g3": 2}), ClassDef2=obj("ClassDef", classDefs={"g2": 1, "g4": 2}),
                Class1Count=3, Class2Count=3, Class1Record=rows)
    anchors = [obj("Anchor", Format=1, XCoordinate=-23, YCoordinate=45), obj("Anchor", Format=2, XCoordinate=17, YCoordinate=-9, AnchorPoint=7),
               obj("Anchor", Format=3, XCoordinate=120, YCoordinate=240, XDeviceTable=devices[2], YDeviceTable=devices[3])]
    cursive = obj("CursivePos", Format=1, Coverage=coverage(["g1", "g3"]), EntryExitCount=2,
                  EntryExitRecord=[obj("EntryExitRecord", EntryAnchor=anchors[0], ExitAnchor=anchors[1]), obj("EntryExitRecord", EntryAnchor=anchors[2], ExitAnchor=None)])
    font["GPOS"] = newTable("GPOS")
    font["GPOS"].table = obj("GPOS", Version=0x10000, ScriptList=obj("ScriptList", ScriptCount=0, ScriptRecord=[]),
        FeatureList=obj("FeatureList", FeatureCount=0, FeatureRecord=[]), LookupList=obj("LookupList", LookupCount=5,
        Lookup=[lookup(1, single1, flags=8), lookup(1, single2), lookup(2, pair1, flags=8), lookup(2, pair2, flags=8), lookup(3, cursive)]))
    gdef = struct.pack(">14H", 1, 0, 12, 0, 0, 0, 1, 0, 5, 0, 1, 1, 3, 1)
    return font, font.getTableData("GPOS"), gdef


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font-manifest", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--fixture-header", type=Path)
    args = parser.parse_args(); args.out.parent.mkdir(parents=True, exist_ok=True)
    active = [None]; positions = {}; values = {}; anchors = {}; original = BaseTable.decompile; read_value = ValueRecordFactory.readValueRecord
    names = {name: mask for mask, name, *_ in valueRecordFormat}
    def observed(self, reader, font):
        tracked = str(reader.tableTag) == "GPOS"
        if tracked: assert bytes(reader.data) == active[0]; positions[id(self)] = reader.pos
        original(self, reader, font)
        if tracked and isinstance(self, ot.Anchor): anchors[positions[id(self)]] = self
    def observed_value(self, reader, font):
        at, parent = reader.pos, reader.offset
        result = read_value(self, reader, font)
        if str(reader.tableTag) == "GPOS":
            mask = sum(names[name] for name, *_ in self.format)
            assert reader.pos-at == mask.bit_count()*2
            values[(parent, at-parent, mask)] = result
        return result
    BaseTable.decompile = observed; ValueRecordFactory.readValueRecord = observed_value
    blobs = []; metadata = []; totals = collections.Counter(); fixture_blob = None
    def emit(font, raw, gd, identity):
        nonlocal fixture_blob
        from fontTools.ttLib.tables.otBase import OTTableReader
        positions.clear(); values.clear(); anchors.clear(); active[0] = raw
        table = ot.GPOS(); table.decompile(OTTableReader(raw, tableTag="GPOS"), font); table.ensureDecompiled(recurse=True)
        limit = len(font.getGlyphOrder()); gid = font.getGlyphID; minimums = {}; entries = []
        for li, l in enumerate(table.LookupList.Lookup):
            for si, wrapped in enumerate(l.SubTable):
                sub = wrapped.ExtSubTable if l.LookupType == 9 else wrapped
                kind = wrapped.ExtensionLookupType if l.LookupType == 9 else l.LookupType
                if kind not in (1, 2): continue
                offset = positions[id(sub)]; mask1 = sub.ValueFormat if kind == 1 else sub.ValueFormat1; mask2 = 0 if kind == 1 else sub.ValueFormat2
                size1, size2 = mask1.bit_count()*2, mask2.bit_count()*2
                if kind == 1: minimums[offset] = 6+size1 if sub.Format == 1 else 8+sub.ValueCount*size1
                elif sub.Format == 1:
                    for group in sub.PairSet: minimums[positions[id(group)]] = 2+group.PairValueCount*(2+size1+size2)
                else: minimums[offset] = 16+sub.Class1Count*sub.Class2Count*(size1+size2)
                entries.append((li, si, l, sub, kind, offset, mask1, mask2, size1, size2))
        keys = sorted(values); indices = {key: i for i, key in enumerate(keys)}
        def ref(parent, offset, mask): return indices[(parent, offset, mask)]
        value_data = []
        for parent, offset, mask in keys:
            assert parent in minimums
            value_data.append(word(parent, offset, mask, minimums[parent]) + value_blob(values[(parent, offset, mask)]))
            totals["value_records"] += 1
            totals[f"value_format_{mask}"] += 1
            for field in DEVICES:
                d = getattr(values[(parent, offset, mask)], field, None)
                if d: totals[f"value_device_format_{d.DeltaFormat}"] += 1
        anchor_data = []
        for at, anchor in sorted(anchors.items()):
            anchor_data.append(struct.pack("<IHhhH", at, anchor.Format, anchor.XCoordinate, anchor.YCoordinate, getattr(anchor, "AnchorPoint", 0)) +
                               device_blob(getattr(anchor, "XDeviceTable", None)) + device_blob(getattr(anchor, "YDeviceTable", None)))
            totals[f"anchor_format_{anchor.Format}"] += 1
        g = font["GDEF"].table if "GDEF" in font else None
        if identity.get("synthetic"):
            from fontTools.ttLib.tables.otBase import OTTableReader
            g = ot.GDEF(); g.decompile(OTTableReader(gd, tableTag="GDEF"), font); g.ensureDecompiled(recurse=True)
        cl = {gid(k): v for k, v in g.GlyphClassDef.classDefs.items()} if g and g.GlyphClassDef else {}
        mc = {gid(k): v for k, v in g.MarkAttachClassDef.classDefs.items()} if g and g.MarkAttachClassDef else {}
        marksets = [{gid(n) for n in c.glyphs} for c in getattr(getattr(g, "MarkGlyphSetsDef", None), "Coverage", [])]
        sub_data = []; probe_count = 0
        for li, si, l, sub, kind, offset, mask1, mask2, size1, size2 in entries:
            cover = {gid(name): i for i, name in enumerate(sub.Coverage.glyphs)}; flags = l.LookupFlag
            def ignored(glyph):
                c = cl.get(glyph, 0)
                if c == 1: return bool(flags & 2)
                if c == 2: return bool(flags & 4)
                if c != 3: return False
                if flags & 8: return True
                if flags & 16: return glyph not in marksets[l.MarkFilteringSet]
                return bool(flags >> 8 and mc.get(glyph, 0) != flags >> 8)
            pair_sets = {}; class1 = {}; class2 = {}
            if kind == 2 and sub.Format == 1:
                for first, ci in cover.items():
                    group = sub.PairSet[ci]; parent = positions[id(group)]; stride = 2+size1+size2
                    pair_sets[first] = {gid(record.SecondGlyph): (ref(parent, 4+i*stride, mask1), ref(parent, 4+i*stride+size1, mask2)) for i, record in enumerate(group.PairValueRecord)}
            elif kind == 2:
                class1 = {gid(k): v for k, v in sub.ClassDef1.classDefs.items()}; class2 = {gid(k): v for k, v in sub.ClassDef2.classDefs.items()}
            probes = []; seen = set()
            def probe(sequence, start=0, end=None):
                if end is None: end = len(sequence)
                key = (tuple(sequence), start, end)
                if key in seen: return
                seen.add(key); first = sequence[start]; found = False; second = 0xffffffff; next_input = 0; refs = (0xffffffff, 0xffffffff)
                if first in cover:
                    if kind == 1:
                        at = 6 if sub.Format == 1 else 8+cover[first]*size1
                        refs = (ref(offset, at, mask1), 0xffffffff); found = True; next_input = start+1
                    else:
                        second = next((i for i in range(start+1, end) if not ignored(sequence[i])), 0xffffffff)
                        if second != 0xffffffff:
                            if sub.Format == 1: refs = pair_sets[first].get(sequence[second], (0xffffffff, 0xffffffff)); found = refs[0] != 0xffffffff
                            else:
                                c1, c2 = class1.get(first, 0), class2.get(sequence[second], 0)
                                if c1 < sub.Class1Count and c2 < sub.Class2Count:
                                    at = 16+(c1*sub.Class2Count+c2)*(size1+size2); refs = (ref(offset, at, mask1), ref(offset, at+size1, mask2)); found = True
                            if found: next_input = second+(1 if mask2 else 0)
                probes.append(word(len(sequence), start, end, int(found), second if found else 0xffffffff, next_input, *refs) + b"".join(struct.pack("<HBB", x, cl.get(x, 0), 0) for x in sequence))
            if kind == 1:
                for glyph in range(limit): probe([glyph])
            else:
                pairs = []
                if sub.Format == 1: pairs = [(a, b) for a, row in pair_sets.items() for b in row]
                else:
                    firsts = {}; seconds = {}
                    for a in cover: firsts.setdefault(class1.get(a, 0), a)
                    for b in range(limit): seconds.setdefault(class2.get(b, 0), b)
                    pairs = [(a, b) for a in firsts.values() for b in seconds.values()]
                skipped = next((x for x in range(limit) if ignored(x)), None)
                for a, b in pairs:
                    probe([a, b]); probe([a, b], end=1)
                    if skipped is not None: probe([a, skipped, b])
                for a in range(limit): probe([a, 0])
            sub_data.append(word(li, si, offset, kind, sub.Format, len(probes)) + b"".join(probes)); probe_count += len(probes)
            totals[f"type_{kind}_format_{sub.Format}"] += 1
        blob = word(limit, len(raw), len(gd), len(keys), len(anchors), len(entries)) + raw + gd + b"".join(value_data) + b"".join(anchor_data) + b"".join(sub_data)
        blobs.append(blob); totals["probes"] += probe_count
        metadata.append({**identity, "values": len(keys), "anchors": len(anchors), "subtables": len(entries), "probes": probe_count, "gpos_sha256": hashlib.sha256(raw).hexdigest()})
        if identity.get("synthetic"): fixture_blob = b"MCGPOS1\0" + word(1) + blob
    for source in json.loads(args.font_manifest.read_text())["fonts"]:
        path = Path(source["path"]); rawfile = path.read_bytes(); assert hashlib.sha256(rawfile).hexdigest() == source["sha256"]
        with TTFont(path, fontNumber=source["face"]) as font:
            if "GPOS" not in font: continue
            assert len(font.getGlyphOrder()) == source["glyphs"]
            emit(font, bytes(font.reader["GPOS"]), bytes(font.reader["GDEF"]) if "GDEF" in font else b"", source)
    font, raw, gd = synthetic(); emit(font, raw, gd, {"synthetic": True, "name": "first-party GPOS values and anchors"})
    args.out.write_bytes(b"MCGPOS1\0" + word(len(blobs)) + b"".join(blobs))
    meta = {"scope": "Original packed values/anchors and independent single/pair matching; not Windows placement/pixel parity", "fonttools": fontTools.__version__, "fonts": metadata, "totals": dict(totals), "corpus_sha256": hashlib.sha256(args.out.read_bytes()).hexdigest()}
    args.out.with_suffix(".json").write_text(json.dumps(meta, indent=2)+"\n")
    if args.fixture_header:
        write_embedded_array(args.fixture_header, "// First-party offline GPOS fixture; generated by gpos_oracle.py.\n",
                             "constexpr u8 gpos_fixture[]", fixture_blob)
    print(json.dumps({"fonts": len(blobs), "bytes": args.out.stat().st_size, **dict(totals)}))


if __name__ == "__main__": main()
