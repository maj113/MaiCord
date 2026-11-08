"""Re-derives the expectations of committed first-party shaping fixtures from HarfBuzz (QA only).

    python tools/font_reference/harfbuzz_fixture.py arabic|component|emoji|emoji_layout|gpos|hebrew|joining|marks|participation|plan [--pins <file>]

Each fixture keeps its fonts and inputs (tests/generated/<name>.bin); only the expected glyphs, clusters and placements
are recomputed with HarfBuzz (uharfbuzz), shaped as the engine is asked to shape them. Every case is kept: one where the
engine knowingly differs is marked known divergent with its class and expects the engine's output, read with --pins
from the test's MAICORD_FIXTURE_RECORD=1 output (pin_* lines). Needs uharfbuzz, numpy and fontTools.
"""
import argparse
import codecs
import functools
import io
import re
import struct
import sys
from pathlib import Path

import numpy as np
import uharfbuzz as hb
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable

from embed_fixture import write_embedded_array

GENERATED = Path(__file__).resolve().parents[2] / 'tests' / 'generated'
ARABIC = ['arabic_plan_fixture', 'arabic_control_fixture', 'arabic_dotted_fixture', 'arabic_cgj_fixture',
          'arabic_ltr_fixture', 'arabic_ltr_cgj_fixture', 'arabic_ltr_cgj_position_fixture', 'arabic_generic_fixture']
INDEPENDENT = {'liga': False, 'clig': False, 'kern': False}


def comment():
    return f'// First-party fonts and inputs; HarfBuzz {hb.version_string()} expectations (harfbuzz_fixture.py).\n'


def bits(value):
    """The float32 bits of a placement value; negative zero is stored as zero."""
    value = np.float32(value)
    return int(np.float32(0).view('<u4')) if value == 0 else int(value.view('<u4'))


@functools.lru_cache(maxsize=None)
def with_positioning(font_bytes):
    """The font with a GPOS when it has none, holding one unused lookup under DFLT and hebr. HarfBuzz positions the
    marks of a font without GPOS lookups from glyph extents (hb_ot_position_fallback), and the Hebrew shaper ignores a
    GPOS without its script (gpos_tag); the engine has no such fallback, so the expectations leave it out."""
    font = TTFont(io.BytesIO(font_bytes))
    if 'GPOS' in font:
        return font_bytes
    addOpenTypeFeaturesFromString(font, 'languagesystem DFLT dflt;\nlanguagesystem hebr dflt;\n'
                                        'lookup unused { pos .notdef 0; } unused;\nfeature test { lookup unused; } test;\n',
                                  tables=['GPOS'])
    output = io.BytesIO()
    font.save(output)
    return output.getvalue()


def shape(font_bytes, scalars, rtl, em_bits, independent, script='arab', bot=True):
    """HarfBuzz's (glyphs, cluster per scalar, placement bits) in the engine's order: logical for right to left,
    left to right otherwise. A scalar maps to the first glyph of its cluster; placements are (advance, x offset,
    y offset) scaled to the em, the x offset negated for right to left as the engine stores it."""
    face = hb.Face(hb.Blob(with_positioning(font_bytes)))
    font = hb.Font(face)
    scale = np.float32(np.frombuffer(struct.pack('<I', em_bits), '<f4')[0]) / np.float32(face.upem)
    buffer = hb.Buffer()
    buffer.add_codepoints(list(scalars))
    buffer.direction = 'rtl' if rtl else 'ltr'
    buffer.script = script
    buffer.language = 'en'
    buffer.flags = hb.BufferFlags.BOT | hb.BufferFlags.EOT if bot else hb.BufferFlags.DEFAULT
    hb.shape(font, buffer, INDEPENDENT if independent else {})
    infos, positions = list(buffer.glyph_infos), list(buffer.glyph_positions)
    if rtl:
        infos.reverse()
        positions.reverse()
    first = {}
    for index, info in enumerate(infos):
        first.setdefault(info.cluster, index)
    starts = sorted(first)
    clusters = [first[max(c for c in starts if c <= s)] for s in range(len(scalars))]
    placements = []
    for p in positions:
        placements += [bits(np.float32(p.x_advance) * scale), bits(np.float32(-p.x_offset if rtl else p.x_offset) * scale),
                       bits(np.float32(p.y_offset) * scale)]
    return [info.codepoint for info in infos], clusters, placements


def read_arabic(data):
    """(version, fonts, cases) of an MCARPL corpus."""
    assert data[:6] == b'MCARPL' and data[7] == 0
    version, at = data[6] - 48, 8

    def word():
        nonlocal at
        at += 4
        return struct.unpack_from('<I', data, at - 4)[0]

    def take(n):
        nonlocal at
        at += n
        return data[at - n:at]

    font_count, case_count = word(), word()
    fonts = [take(word()) for _ in range(font_count)]
    cases = []
    for _ in range(case_count):
        em_bits = word() if version >= 2 else 1148846080
        rtl = word() if version >= 3 else 1
        divergence = word() if version >= 4 else 0
        font, independent, n, expected = word(), word(), word(), word()
        scalars = list(struct.unpack(f'<{n}I', take(4 * n)))
        take(2 * expected + 4 * n + 12 * expected)
        cases.append(dict(em_bits=em_bits, rtl=rtl, divergence=divergence, font=font, independent=independent, scalars=scalars))
    assert at == len(data)
    return version, fonts, cases


GPOS = ['gpos_execute_fixture', 'gpos_mark_execute_fixture', 'gpos_cursive_execute_fixture', 'gpos_ligature_execute_fixture']


@functools.lru_cache(maxsize=None)
def glyph_addressed(font_bytes):
    """The font with a cmap that maps U+F0000 + glyph ID to each glyph, so HarfBuzz can shape a glyph sequence."""
    font = TTFont(io.BytesIO(font_bytes))
    table = CmapSubtable.newSubtable(12)
    table.platformID, table.platEncID, table.language = 3, 10, 0
    table.cmap = {0xF0000 + glyph: name for glyph, name in enumerate(font.getGlyphOrder())}
    font['cmap'].tables = [t for t in font['cmap'].tables if (t.platformID, t.platEncID) != (3, 10)] + [table]
    output = io.BytesIO()
    font.save(output)
    return output.getvalue()


def position(font_bytes, glyphs, rtl, em_bits, test=True):
    """HarfBuzz's (glyphs, placement bits) for a glyph sequence, the font's 'test' feature on or off, in logical order.
    Right to left shapes as Arabic: HarfBuzz reverses the graphemes of a left-to-right script set right to left
    (hb_ensure_native_direction), and the Arabic shaper's joining and mark rules leave private-use glyph addresses
    (joining type U, combining class 0) alone."""
    face = hb.Face(hb.Blob(glyph_addressed(font_bytes)))
    font = hb.Font(face)
    scale = np.float32(np.frombuffer(struct.pack('<I', em_bits), '<f4')[0]) / np.float32(face.upem)
    buffer = hb.Buffer()
    buffer.add_codepoints([0xF0000 + glyph for glyph in glyphs])
    buffer.direction = 'rtl' if rtl else 'ltr'
    buffer.script = 'Arab' if rtl else 'Latn'
    buffer.language = 'en'
    hb.shape(font, buffer, {'test': test, 'tlig': True})
    infos, positions = list(buffer.glyph_infos), list(buffer.glyph_positions)
    if rtl:
        infos.reverse()
        positions.reverse()
    placements = []
    for p in positions:
        placements += [bits(np.float32(p.x_advance) * scale), bits(np.float32(-p.x_offset if rtl else p.x_offset) * scale),
                       bits(np.float32(p.y_offset) * scale)]
    return [info.codepoint for info in infos], struct.pack(f'<{len(placements)}I', *placements)


def ligature_input(font_bytes, glyphs, targets, associations):
    """(font, glyph sequence) through which HarfBuzz forms the ligature corpus's seeded attachments, or None: each
    target ligature is ligated by a 'tlig' lookup (IgnoreMarks) from as many copies of itself as its mark-to-ligature
    ComponentCount, with each mark seeded to an earlier component placed after that copy, so the mark carries the
    component (lig_comp). Marks of the last or of an unknown component follow the ligature. None where a seed names
    a component the ligature lacks (the engine skips such a mark)."""
    font = TTFont(io.BytesIO(font_bytes))
    names = font.getGlyphOrder()
    counts = {}
    for lookup in font['GPOS'].table.LookupList.Lookup:
        for subtable in lookup.SubTable:
            subtable = getattr(subtable, 'ExtSubTable', subtable)
            if hasattr(subtable, 'LigatureArray'):
                for name, attach in zip(subtable.LigatureCoverage.glyphs, subtable.LigatureArray.LigatureAttach):
                    counts[name] = attach.ComponentCount
    inside = {}
    for index, (slot, component) in enumerate(associations):
        if slot == 0xFFFFFFFF or component == 0xFFFFFFFF:
            continue
        target = targets[slot]
        count = counts.get(names[glyphs[target]], 1)
        if component >= count:
            return None
        if component < count - 1:
            inside.setdefault(target, []).append((component, index))
    moved = {index for marks in inside.values() for _, index in marks}
    sequence, ligatures = [], {}
    for index, glyph in enumerate(glyphs):
        if index in moved:
            continue
        if index not in inside:
            sequence.append(glyph)
            continue
        ligatures[names[glyph]] = counts[names[glyph]]
        for component in range(counts[names[glyph]]):
            sequence.append(glyph)
            sequence += [glyphs[mark] for seeded, mark in sorted(inside[index]) if seeded == component]
    if ligatures:
        rules = ' '.join(f'sub {" ".join([name] * count)} by {name};' for name, count in ligatures.items())
        addOpenTypeFeaturesFromString(font, 'languagesystem DFLT dflt;\nlanguagesystem arab dflt;\nlanguagesystem latn dflt;\n'
                                      f'lookup tlig {{ lookupflag IgnoreMarks; {rules} }} tlig;\nfeature tlig {{ lookup tlig; }} tlig;\n',
                                      tables=['GSUB'])
        output = io.BytesIO()
        font.save(output)
        font_bytes = output.getvalue()
    return font_bytes, sequence


def covered_non_marks(font, glyphs):
    """Whether a glyph of the sequence is in a mark attachment's mark coverage without the GDEF mark class: HarfBuzz
    attaches such a glyph (the coverage alone selects the mark), the engine attaches GDEF marks only."""
    classes = font['GDEF'].table.GlyphClassDef.classDefs if 'GDEF' in font and font['GDEF'].table.GlyphClassDef else {}
    covered = set()
    for lookup in font['GPOS'].table.LookupList.Lookup:
        for subtable in lookup.SubTable:
            subtable = getattr(subtable, 'ExtSubTable', subtable)
            for field in ('MarkCoverage', 'Mark1Coverage'):
                if getattr(subtable, field, None):
                    covered |= set(getattr(subtable, field).glyphs)
    order = font.getGlyphOrder()
    return any(order[glyph] in covered and classes.get(order[glyph]) != 3 for glyph in glyphs)


def lookup_type(lookup):
    return lookup.SubTable[0].ExtensionLookupType if lookup.LookupType == 9 else lookup.LookupType


@functools.lru_cache(maxsize=None)
def restricted(font_bytes, lookups):
    """The font with its 'test' feature holding only the given lookups."""
    font = TTFont(io.BytesIO(font_bytes))
    for record in font['GPOS'].table.FeatureList.FeatureRecord:
        if record.FeatureTag == 'test':
            record.Feature.LookupListIndex = [index for index in record.Feature.LookupListIndex if index in lookups]
            record.Feature.LookupCount = len(record.Feature.LookupListIndex)
    output = io.BytesIO()
    font.save(output)
    return output.getvalue()


def font_units(font_bytes, glyphs, rtl, test=True):
    """HarfBuzz's (advance, x offset, y offset) in font units per glyph of a glyph sequence, in logical order."""
    font = hb.Font(hb.Face(hb.Blob(glyph_addressed(font_bytes))))
    buffer = hb.Buffer()
    buffer.add_codepoints([0xF0000 + glyph for glyph in glyphs])
    buffer.direction, buffer.script, buffer.language = ('rtl', 'Arab', 'en') if rtl else ('ltr', 'Latn', 'en')
    hb.shape(font, buffer, {'test': test, 'tlig': True})
    positions = [(p.x_advance, p.x_offset, p.y_offset) for p in buffer.glyph_positions]
    return positions[::-1] if rtl else positions


def deferred_attachment(font_bytes, glyphs, rtl):
    """Whether HarfBuzz's placements depend on resolving attachments after every lookup (propagate_attachment_offsets):
    a later lookup that moves an attachment's parent or the glyphs between moves the attached glyph too, and a mark's
    advance is zeroed before attachments resolve, while the engine fixes an attachment when its lookup applies.
    Detected by a single or pair lookup that can give a GDEF mark of the sequence an advance, by a contextual lookup
    beside an attachment (its nested actions attach in its own order), and by comparing HarfBuzz's result with the
    lookups up to the first attachment plus the separate effect of the later single and pair lookups."""
    font = TTFont(io.BytesIO(font_bytes))
    table = font['GPOS'].table
    order = sorted({index for record in table.FeatureList.FeatureRecord if record.FeatureTag == 'test'
                    for index in record.Feature.LookupListIndex})
    kinds = [lookup_type(table.LookupList.Lookup[index]) for index in order]
    every = {lookup_type(lookup) for lookup in table.LookupList.Lookup}
    if every & {3, 4, 5, 6} and every & {7, 8}:
        return True
    first = next((i for i, kind in enumerate(kinds) if kind in (3, 4, 5, 6)), None)
    if first is None:
        return False
    classes = font['GDEF'].table.GlyphClassDef.classDefs if 'GDEF' in font and font['GDEF'].table.GlyphClassDef else {}
    names = font.getGlyphOrder()
    marks = {names[glyph] for glyph in glyphs if classes.get(names[glyph]) == 3}
    for index, kind in zip(order, kinds):
        for subtable in table.LookupList.Lookup[index].SubTable:
            subtable = getattr(subtable, 'ExtSubTable', subtable)
            if kind == 1 and subtable.ValueFormat & 4 and marks & set(subtable.Coverage.glyphs):
                return True
            if kind == 2 and marks and (subtable.ValueFormat1 | subtable.ValueFormat2) & 4:
                return True
    # A cursive lookup after a mark attachment changes the advances the mark's offset spans.
    first_mark = next((i for i, kind in enumerate(kinds) if kind in (4, 5, 6)), None)
    if first_mark is not None and 3 in kinds[first_mark + 1:]:
        return True
    late = frozenset(index for index, kind in zip(order[first + 1:], kinds[first + 1:]) if kind not in (3, 4, 5, 6))
    if not late:
        return False
    full = font_units(font_bytes, glyphs, rtl)
    early = font_units(restricted(font_bytes, frozenset(order) - late), glyphs, rtl)
    alone = font_units(restricted(font_bytes, late), glyphs, rtl)
    none = font_units(font_bytes, glyphs, rtl, False)
    return any(f != tuple(e[k] + a[k] - n[k] for k in range(3)) for f, e, a, n in zip(full, early, alone, none))


# Known divergence classes of the positioning corpora (MCG..02): the font's final placements are the engine's.
GPOS_DIVERGENCES = {1: 'mark coverage without the GDEF mark class', 2: 'deferred attachment',
                    3: 'seeded ligature HarfBuzz cannot form'}


def read_pins(path, kind):
    """The engine values a test printed for known divergent cases ('pin_<kind> key=value ...' lines), by key tuple."""
    pins = {}
    if path:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith(f'pin_{kind} '):
                fields = dict(item.split('=') for item in line.split()[1:])
                value = int(fields.pop('actual'))
                pins[tuple(int(fields[name]) for name in sorted(fields))] = value
    return pins


def gpos(pins_path=None):
    """Basic, mark, cursive and ligature positioning corpora (MCGPX02, MCGMK02, MCGCU02, MCGLP02; reads 01 or 02): each
    configuration's final placements are HarfBuzz's for the glyph sequence with the font's 'test' lookups, in its
    direction (the ligature corpus's seeded components through ligature_input); the initial placements are the nominal
    advances, zero for a glyph flagged zero-advance, without offsets. A font where the engine knowingly may differ is
    known divergent (GPOS_DIVERGENCES, one word after the font header): it covers a non-mark glyph of its sequence as a
    mark, its result depends on deferred attachment (see covered_non_marks and deferred_attachment), or HarfBuzz cannot
    form its seeded ligature. Its final placements are the engine's, from the test's pin_position lines (--pins,
    MAICORD_FIXTURE_RECORD=1; the test prints a pin where the engine differs from the fixture), else HarfBuzz's (the
    recorded ones without a HarfBuzz result). Regenerate without --pins, record, then regenerate with them: a font
    with no pin then agrees with HarfBuzz and is not divergent."""
    for corpus, name in enumerate(GPOS):
        data = (GENERATED / f'{name}.bin').read_bytes()
        version = data[6] - 48
        assert data[:6] in (b'MCGPX0', b'MCGMK0', b'MCGCU0', b'MCGLP0') and version in (1, 2) and data[7] == 0
        ligature = data[:6] == b'MCGLP0'
        pins = read_pins(pins_path, 'position')  # keyed by (component, config, corpus, font, glyph)
        pinned_fonts = {(key[2], key[3]) for key in pins}
        at, out, counts = 12, bytearray(data[:6] + b'2\0' + data[8:12]), [0] * (len(GPOS_DIVERGENCES) + 1)
        for font_index in range(struct.unpack_from('<I', data, 8)[0]):
            size, n, lookups, configurations = struct.unpack_from('<4I', data, at)
            head = at + 16 + (4 if version == 2 else 0)
            seeds = struct.unpack_from('<I', data, head)[0] if ligature else 0
            head += 4 if ligature else 0
            raw = data[head:head + size]
            records = [struct.unpack_from('<HBB', data, head + size + 4 * i) for i in range(n)]
            glyphs = [record[0] for record in records]
            targets = struct.unpack_from(f'<{seeds}I', data, head + size + 4 * n)
            associations = [struct.unpack_from('<2I', data, head + size + 4 * n + 4 * seeds + 8 * i) for i in range(n)] if seeds else []
            end = head + size + 4 * n + 4 * seeds + (8 * n if seeds else 0) + 2 * lookups
            font = TTFont(io.BytesIO(raw))
            order, upem = font.getGlyphOrder(), font['head'].unitsPerEm
            shaped_font, sequence = ligature_input(raw, glyphs, targets, associations) or (None, None)
            directions = {struct.unpack_from('<I', data, end + c * (8 + 24 * n) + 4)[0] for c in range(configurations)}
            if not shaped_font or any(position(shaped_font, sequence, rtl, 1148846080)[0] != glyphs for rtl in directions):
                divergence = 3
            elif covered_non_marks(font, glyphs):
                divergence = 1
            elif any(deferred_attachment(shaped_font, sequence, rtl) for rtl in directions):
                divergence = 2
            else:
                divergence = 0
            if divergence in (1, 2) and pins_path and (corpus, font_index) not in pinned_fonts:
                divergence = 0
            counts[divergence] += 1
            out += struct.pack('<5I', size, n, lookups, configurations, divergence)
            out += (struct.pack('<I', seeds) if ligature else b'') + data[head:end]
            at = end
            for config in range(configurations):
                em_bits, rtl = struct.unpack_from('<2I', data, at)
                scale = np.float32(np.frombuffer(struct.pack('<I', em_bits), '<f4')[0]) / np.float32(upem)
                initial = [value for glyph, _, flags in records for value in
                           (0 if flags & 1 else bits(np.float32(font['hmtx'][order[glyph]][0]) * scale), 0, 0)]
                if divergence == 3:
                    final = list(struct.unpack_from(f'<{3 * n}I', data, at + 8 + 12 * n))
                else:
                    shaped, placements = position(shaped_font, sequence, rtl, em_bits)
                    assert shaped == glyphs
                    final = list(struct.unpack(f'<{3 * n}I', placements))
                if divergence:
                    for word in range(3 * n):
                        final[word] = pins.get((word % 3, config, corpus, font_index, word // 3), final[word])
                out += data[at:at + 8] + struct.pack(f'<{3 * n}I', *initial) + struct.pack(f'<{3 * n}I', *final)
                at += 8 + 24 * n
        assert at == len(data)
        write_embedded_array(GENERATED / f'{name}.inc', comment(), 'constexpr u8 position_fixture[]', bytes(out))
        print(name, 'regenerated,', sum(counts), 'fonts,', ', '.join(f'{counts[k]} known divergent ({v})' for k, v in GPOS_DIVERGENCES.items()))


def past_input(font):
    """Whether a contextual GPOS lookup nests a lookup that matches more than one glyph (pair, cursive, contextual).
    HarfBuzz lets a nested lookup match past the context's input; the engine keeps nested matching inside it."""
    lookups = font['GPOS'].table.LookupList.Lookup
    for lookup in lookups:
        for subtable in lookup.SubTable:
            subtable = getattr(subtable, 'ExtSubTable', subtable)
            rules = []
            if getattr(subtable, 'Format', 0) == 3:
                rules.append(subtable)
            for name in ('PosRuleSet', 'PosClassSet', 'ChainPosRuleSet', 'ChainPosClassSet'):
                for rule_set in getattr(subtable, name, None) or []:
                    for rule_name in ('PosRule', 'PosClassRule', 'ChainPosRule', 'ChainPosClassRule'):
                        rules += getattr(rule_set, rule_name, None) or [] if rule_set else []
            for rule in rules:
                for record in getattr(rule, 'PosLookupRecord', None) or []:
                    if lookup_type(lookups[record.LookupListIndex]) in (2, 3, 7, 8):
                        return True
    return False


def grapheme_starts(scalars):
    """The scalar indices where HarfBuzz starts a cluster in the text itself (hb_form_clusters: marks and joiners join
    their base), shaped with an empty font."""
    font = hb.Font(hb.Face(b''))
    buffer = hb.Buffer()
    buffer.add_codepoints(list(scalars))
    buffer.guess_segment_properties()
    buffer.direction = 'ltr'
    hb.shape(font, buffer, {})
    return {info.cluster for info in buffer.glyph_infos}


def component_clusters(font_bytes, scalars, nominal, rtl):
    """HarfBuzz's (glyphs, first glyph per source unit, position flags) for the component corpus: the clusters of the
    text (grapheme_starts) joined with those GSUB merges on the nominal glyph sequence (position's shaping), as
    sfnt_shape_run joins its initial graphemes with GSUB's merges; a decomposed ligature keeps its merged cluster. A flag
    is 2 on a cluster's first glyph in glyph order, plus 1 on a GDEF mark."""
    face = hb.Face(hb.Blob(glyph_addressed(with_positioning(font_bytes))))
    buffer = hb.Buffer()
    buffer.add_codepoints([0xF0000 + glyph for glyph in nominal])
    buffer.direction = 'rtl' if rtl else 'ltr'
    buffer.script = 'Arab' if rtl else 'Latn'
    buffer.language = 'en'
    hb.shape(hb.Font(face), buffer, {'test': True, 'tlig': True})
    infos = list(buffer.glyph_infos)
    if rtl:
        infos.reverse()
    starts = sorted((grapheme_starts(scalars) & {info.cluster for info in infos}) | {0})
    cluster = [0] * len(scalars)
    for index, start in enumerate(starts):
        for i in range(start, starts[index + 1] if index + 1 < len(starts) else len(scalars)):
            cluster[i] = index
    glyph_clusters = [cluster[info.cluster] for info in infos]
    first = {}
    for glyph, value in enumerate(glyph_clusters):
        first.setdefault(value, glyph)
    characters = [first[cluster[i]] for i, scalar in enumerate(scalars) for _ in range(2 if scalar > 0xFFFF else 1)]
    font = TTFont(io.BytesIO(font_bytes))
    classes = font['GDEF'].table.GlyphClassDef.classDefs if 'GDEF' in font and font['GDEF'].table.GlyphClassDef else {}
    names, seen, flags = font.getGlyphOrder(), set(), []
    for info, value in zip(infos, glyph_clusters):
        flags.append((0 if value in seen else 2) | (1 if classes.get(names[info.codepoint]) == 3 else 0))
        seen.add(value)
    return [info.codepoint for info in infos], characters, flags


# Known divergence classes of the component corpus (MCCMP02): the font's placements are the engine's.
COMPONENT_DIVERGENCES = {1: 'mark coverage without the GDEF mark class', 2: 'miscounted ligature',
                         3: 'marks after a multiple substitution', 4: 'multi-glyph lookup nested in a context',
                         5: 'deferred attachment'}


def component(pins_path=None):
    """component_shaping_fixture (MCCMP02; reads 01 or 02, maicord_native_component_shaping): each configuration's
    placements without and with the font's 'test' GPOS feature are HarfBuzz's, shaping the nominal glyphs of the text
    (glyph addressed, as position does) so that GSUB 'rlig' and GPOS see the unnormalized sequence sfnt_shape_run
    takes. HarfBuzz's glyphs must equal the kept glyphs; where they do, the cluster map and position flags are
    HarfBuzz's too (see component_clusters), else the kept ones. A font is known divergent
    (COMPONENT_DIVERGENCES, one word after the font header) where it covers a non-mark glyph of its output as a mark,
    shows a miscounted ligature (see covered_non_marks, miscounted_ligatures), attaches marks after a multiple
    substitution, whose later outputs HarfBuzz never takes as attachment targets (MarkBasePos accept) while the engine
    does, nests a multi-glyph lookup in a context (see past_input) or depends on deferred attachment (see
    deferred_attachment). Its placements are the engine's, from the test's pin_component lines (--pins,
    MAICORD_FIXTURE_RECORD=1; the test prints a pin where the engine differs from the fixture), else HarfBuzz's (the
    recorded ones where HarfBuzz forms other glyphs). Regenerate without --pins, record, then regenerate with them: a
    font with no pin then agrees with HarfBuzz and is not divergent."""
    data = (GENERATED / 'component_shaping_fixture.bin').read_bytes()
    assert data[:8] in (b'MCCMP01\0', b'MCCMP02\0')
    header = 24 if data[:8] == b'MCCMP02\0' else 20
    pins = read_pins(pins_path, 'component')  # keyed by (config, font, stage, word)
    pinned_fonts = {key[1] for key in pins}
    at, out, counts = 12, bytearray(b'MCCMP02\0' + data[8:12]), [0] * (len(COMPONENT_DIVERGENCES) + 1)
    for font_index in range(struct.unpack_from('<I', data, 8)[0]):
        size, count, units, n, configurations = struct.unpack_from('<5I', data, at)
        raw = data[at + header:at + header + size]
        scalars = struct.unpack_from(f'<{count}I', data, at + header + size)
        records = at + header + size + 4 * count + 4 * units
        glyphs = [struct.unpack_from('<H', data, records + 4 * i)[0] for i in range(n)]
        font = TTFont(io.BytesIO(raw))
        ids = {name: index for index, name in enumerate(font.getGlyphOrder())}
        cmap = font.getBestCmap()
        nominal = [ids.get(cmap.get(scalar), 0) for scalar in scalars]
        end = records + 4 * n
        directions = {struct.unpack_from('<I', data, end + c * (8 + 24 * n) + 4)[0] for c in range(configurations)}
        attaches = any(lookup_type(lookup) in (4, 5, 6) for lookup in font['GPOS'].table.LookupList.Lookup)
        if covered_non_marks(font, glyphs):
            divergence = 1
        elif miscounted_ligatures(raw) & set(glyphs):
            divergence = 2
        elif multiplies(raw) and attaches:
            divergence = 3
        elif past_input(font):
            divergence = 4
        elif any(deferred_attachment(with_positioning(raw), nominal, rtl) for rtl in directions):
            divergence = 5
        else:
            divergence = 0
        # Without a pin (the engine matched the fixture), a font HarfBuzz shapes to its glyphs agrees.
        agrees, formed = bool(pins_path) and font_index not in pinned_fonts, True
        header_at = len(out)
        body = bytearray(data[at + header:end])
        clustered, characters, flags = component_clusters(raw, scalars, nominal, min(directions))
        if clustered == glyphs:
            struct.pack_into(f'<{units}I', body, size + 4 * count, *characters)
            for i, flag in enumerate(flags):
                body[size + 4 * count + 4 * units + 4 * i + 2] = flag
        out += struct.pack('<6I', size, count, units, n, configurations, divergence) + body
        at = end
        for config in range(configurations):
            em_bits, rtl = struct.unpack_from('<2I', data, at)
            out += data[at:at + 8]
            for stage, test in enumerate((False, True)):
                shaped, placements = position(with_positioning(raw), nominal, rtl, em_bits, test)
                formed = formed and shaped == glyphs
                if divergence and shaped != glyphs:
                    placements = data[at + 8 + 12 * n * stage:at + 8 + 12 * n * (stage + 1)]
                assert divergence or shaped == glyphs
                words = list(struct.unpack(f'<{3 * n}I', placements))
                if divergence:
                    words = [pins.get((config, font_index, stage, word), value) for word, value in enumerate(words)]
                out += struct.pack(f'<{3 * n}I', *words)
            at += 8 + 24 * n
        if divergence and agrees and formed:
            divergence = 0
            struct.pack_into('<I', out, header_at + 20, 0)
        counts[divergence] += 1
    assert at == len(data)
    write_embedded_array(GENERATED / 'component_shaping_fixture.inc', comment(), 'constexpr u8 component_fixture[]', bytes(out))
    print('component_shaping_fixture regenerated,', sum(counts), 'fonts,',
          ', '.join(f'{counts[k]} known divergent ({v})' for k, v in COMPONENT_DIVERGENCES.items()))


# HarfBuzz's default ignorables (hb_unicode_funcs_t::is_default_ignorable).
IGNORABLE = [(0x00AD, 0x00AD), (0x034F, 0x034F), (0x061C, 0x061C), (0x115F, 0x1160), (0x17B4, 0x17B5), (0x180B, 0x180F),
             (0x200B, 0x200F), (0x202A, 0x202E), (0x2060, 0x206F), (0x3164, 0x3164), (0xFE00, 0xFE0F), (0xFEFF, 0xFEFF),
             (0xFFA0, 0xFFA0), (0xFFF0, 0xFFF8), (0x1BCA0, 0x1BCA3), (0x1D173, 0x1D17A), (0xE0000, 0xE0FFF)]


def ignorable(code_point):
    return any(first <= code_point <= last for first, last in IGNORABLE)


@functools.lru_cache(maxsize=None)
def font_traits(font_bytes):
    """(GPOS features, GPOS has a masked feature) of a font."""
    font = TTFont(io.BytesIO(font_bytes))
    gpos = font['GPOS'].table.FeatureList if 'GPOS' in font and font['GPOS'].table.FeatureList else None
    masked = bool(gpos) and any(r.FeatureTag in ('isol', 'fina', 'medi', 'init', 'rtlm') for r in gpos.FeatureRecord)
    return bool(gpos), masked


# Known divergence classes of the Arabic corpora (MCARPL4): the case's output is the engine's.
ARABIC_DIVERGENCES = {1: 'masked GPOS feature', 2: 'GPOS meets a default ignorable other than a joiner'}


def arabic_divergence(font_bytes, scalars):
    """The class (ARABIC_DIVERGENCES) where the engine knowingly may differ from HarfBuzz on a case, else 0: the font's
    GPOS has a masked feature (isol, fina, medi, init, rtlm), which the engine does not apply, or meets a default
    ignorable other than ZWJ and ZWNJ, which HarfBuzz's positioning skips and the engine's does not."""
    positions, masked = font_traits(font_bytes)
    if masked:
        return 1
    return 2 if positions and any(ignorable(s) and s not in (0x200C, 0x200D) for s in scalars) else 0


def read_shaped_pins(path, kind):
    """The engine outputs a test printed for known divergent cases (pin_<kind> lines of comma-separated lists), by
    (corpus, case)."""
    pins = {}
    if path:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith(f'pin_{kind} '):
                fields = dict(item.split('=') for item in line.split()[1:])
                pins[int(fields.pop('corpus')), int(fields.pop('case'))] = {
                    name: [int(v) for v in value.split(',')] if value else [] for name, value in fields.items()}
    return pins


def arabic(pins_path=None):
    """MCARPL fixtures (written as MCARPL4: em, direction and divergence per case): each case shaped as Arabic in its
    direction, with the independent-letter features off when asked. A right-to-left case begins its paragraph
    (HarfBuzz's BOT, so a leading mark follows a dotted circle) unless it starts with a default ignorable, which the
    engine keeps invisible; a left-to-right case does not, as the engine inserts no circle into a reversed run. A case
    where the engine knowingly may differ (see arabic_divergence) is marked known divergent until pins are read; with
    --pins (the test's pin_arabic lines, MAICORD_FIXTURE_RECORD=1, both test modes) it stays divergent, expecting the
    engine's output, only where its pin differs from HarfBuzz (a case without a pin was checked against HarfBuzz)."""
    pins = read_shaped_pins(pins_path, 'arabic')
    for corpus, name in enumerate(ARABIC):
        version, fonts, cases = read_arabic((GENERATED / f'{name}.bin').read_bytes())
        records, counts = [], [0] * (len(ARABIC_DIVERGENCES) + 1)
        for index, case in enumerate(cases):
            divergence = arabic_divergence(fonts[case['font']], case['scalars'])
            bot = bool(case['rtl']) and not (case['scalars'] and ignorable(case['scalars'][0]))
            glyphs, clusters, placements = shape(fonts[case['font']], case['scalars'], case['rtl'], case['em_bits'],
                                                 case['independent'], bot=bot)
            if divergence and pins_path:
                pinned = pins.get((corpus, index))
                if pinned is None or (pinned['glyphs'], pinned['clusters'], pinned['placements']) == (list(glyphs), list(clusters), list(placements)):
                    divergence = 0
                else:
                    glyphs, clusters, placements = pinned['glyphs'], pinned['clusters'], pinned['placements']
            counts[divergence] += 1
            fields = [case['em_bits'], case['rtl'], divergence, case['font'], case['independent'], len(case['scalars']), len(glyphs)]
            records.append(struct.pack(f'<{len(fields)}I', *fields) + struct.pack(f'<{len(case["scalars"])}I', *case['scalars']) +
                           struct.pack(f'<{len(glyphs)}H', *glyphs) + struct.pack(f'<{len(clusters)}I', *clusters) +
                           struct.pack(f'<{len(placements)}I', *placements))
        blob = b'MCARPL4\0' + struct.pack('<2I', len(fonts), len(records))
        blob += b''.join(struct.pack('<I', len(font)) + font for font in fonts) + b''.join(records)
        write_embedded_array(GENERATED / f'{name}.inc', comment(), f'constexpr u8 {name}[]', blob)
        print(name, len(records), 'cases,', ', '.join(f'{counts[k]} known divergent ({v})' for k, v in ARABIC_DIVERGENCES.items()))


def mark_font(code_points):
    """A font mapping each code point (and the space) to its own glyph, without layout tables."""
    names = ['.notdef', 'space'] + [f'u{c:04X}' for c in sorted(code_points)]
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(names)
    builder.setupCharacterMap({0x20: 'space', **{c: f'u{c:04X}' for c in code_points}})
    builder.setupGlyf({name: TTGlyphPen(None).glyph() for name in names})
    builder.setupHorizontalMetrics({name: (500, 0) for name in names})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable({'familyName': 'MarkOrder', 'styleName': 'Regular'})
    builder.setupOS2()
    builder.setupPost()
    output = io.BytesIO()
    builder.save(output)
    return output.getvalue(), names


def marks():
    """arabic_marks_fixture.inc: the order HarfBuzz gives each mark sequence when it shapes it as Arabic (normalization's
    sort by modified combining class, then reorder_marks_arabic), CGJ left out as the test projects it out."""
    path = GENERATED / 'arabic_marks_fixture.inc'
    text = path.read_text(encoding='utf-8')
    values = [int(v, 16) for v in text[text.index('[]{') + 3:text.rindex('}')].replace('\n', '').split(',') if v.strip()]
    cases, at = [], 0
    while at < len(values):
        n, expected = values[at], values[at + 1]
        cases.append(values[at + 2:at + 2 + n])
        at += 2 + n + expected
    font_bytes, names = mark_font({c for case in cases for c in case})
    face = hb.Face(hb.Blob(font_bytes))
    font = hb.Font(face)
    out = []
    for case in cases:
        buffer = hb.Buffer()
        buffer.add_codepoints(case)
        buffer.direction, buffer.script, buffer.language = 'rtl', 'arab', 'en'
        hb.shape(font, buffer, {})
        order = [int(names[info.codepoint][1:], 16) for info in reversed(buffer.glyph_infos) if names[info.codepoint] != 'space']
        order = [c for c in order if c != 0x34F]
        assert sorted(order) == sorted(c for c in case if c != 0x34F)
        out += [len(case), len(order)] + case + order
    lines = [','.join(f'0x{v:x}' for v in out[i:i + 16]) + ',' for i in range(0, len(out), 16)]
    path.write_text(f'// HarfBuzz {hb.version_string()} Arabic mark order (harfbuzz_fixture.py marks); CGJ is projected out in this\n'
                    f'// ordering-only test.\nconstexpr u32 arabic_mark_cases={len(cases)};\nconstexpr u32 arabic_mark_vectors[]{{\n' +
                    '\n'.join(lines) + '\n};\n', encoding='utf-8')
    print('arabic_marks_fixture', len(cases), 'cases')


FORMS = {'init': 8, 'medi': 4, 'fina': 2, 'isol': 1}


def joining():
    """arabic_joining_fixture.inc: the joining form HarfBuzz gives each character of each sequence, read from a font
    whose isol/fina/medi/init features map every character to its own form glyph."""
    path = GENERATED / 'arabic_joining_fixture.inc'
    text = path.read_text(encoding='utf-8')
    values = [int(v, 16) for v in text[text.index('[]{') + 3:text.rindex('}')].replace('\n', '').split(',') if v.strip()]
    cases, at = [], 0
    while at < len(values):
        cases.append([v & 0x1FFFFF for v in values[at + 1:at + 1 + values[at]]])
        at += 1 + values[at]
    code_points = sorted({c for case in cases for c in case})
    base = {c: f'u{c:04X}' for c in code_points}
    names = ['.notdef', 'space'] + list(base.values()) + [f'{base[c]}.{tag}' for c in code_points for tag in FORMS]
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(names)
    builder.setupCharacterMap({0x20: 'space', **base})
    builder.setupGlyf({name: TTGlyphPen(None).glyph() for name in names})
    builder.setupHorizontalMetrics({name: (500, 0) for name in names})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable({'familyName': 'Joining', 'styleName': 'Regular'})
    builder.setupOS2()
    builder.setupPost()
    addOpenTypeFeaturesFromString(builder.font, 'languagesystem arab dflt;\n' + ''.join(
        f'feature {tag} {{\n' + ''.join(f'  sub {base[c]} by {base[c]}.{tag};\n' for c in code_points) + f'}} {tag};\n' for tag in FORMS))
    output = io.BytesIO()
    builder.save(output)
    font = hb.Font(hb.Face(hb.Blob(output.getvalue())))
    vectors = []
    for case in cases:
        buffer = hb.Buffer()
        buffer.add_codepoints(case)
        buffer.direction, buffer.script, buffer.language = 'rtl', 'arab', 'en'
        hb.shape(font, buffer, {})
        # The font neither ligates nor decomposes: the glyphs, in logical order, are the characters'.
        glyphs = [names[info.codepoint] for info in reversed(buffer.glyph_infos)]
        assert len(glyphs) == len(case)
        forms = [FORMS.get(name.rpartition('.')[2], 0) for name in glyphs]
        vectors += [len(case)] + [form << 21 | c for c, form in zip(case, forms)]
    lines = ['    ' + ','.join(f'0x{v:x}' for v in vectors[i:i + 12]) + ',' for i in range(0, len(vectors), 12)]
    path.write_text(f'// HarfBuzz {hb.version_string()} Arabic joining forms (harfbuzz_fixture.py joining).\n'
                    f'constexpr u32 arabic_joining_case_count={len(cases)};\n'
                    f'constexpr u32 arabic_joining_scalar_count={sum(len(c) for c in cases)};\n'
                    'constexpr u32 arabic_joining_vectors[]{\n' + '\n'.join(lines) + '\n};\n', encoding='utf-8')
    print('arabic_joining_fixture', len(cases), 'cases')


def emoji_case(font_bytes, weight, text, em_bits):
    """HarfBuzz's (glyphs, placements, UTF-16 map, flags, diacritics) of one left-to-right emoji run beginning its
    paragraph, in the layout of the MCEMOJI3 corpus. A flag holds 2 on a cluster's first glyph and 1 on a glyph with
    no advance of its own (a GDEF mark or a hidden default ignorable); a diacritic is a GDEF mark."""
    face = hb.Face(hb.Blob(font_bytes))
    font = hb.Font(face)
    if weight:
        font.set_variations({'wght': weight})
    scale = np.float32(np.frombuffer(struct.pack('<I', em_bits), '<f4')[0]) / np.float32(face.upem)
    buffer = hb.Buffer()
    buffer.add_str(text)
    buffer.direction, buffer.language = 'ltr', 'en'
    buffer.flags = hb.BufferFlags.BOT | hb.BufferFlags.EOT
    buffer.guess_segment_properties()
    hb.shape(font, buffer, {})
    infos, positions = list(buffer.glyph_infos), list(buffer.glyph_positions)
    code_points = [ord(c) for c in text]
    units, unit = [], 0
    for c in code_points:
        units.append(unit)
        unit += 2 if c > 0xFFFF else 1
    space = font.get_nominal_glyph(0x20) or 0
    first, flags, marks = {}, [], []
    bounds = sorted({info.cluster for info in infos}) + [len(code_points)]
    for index, info in enumerate(infos):
        mark = face.get_layout_glyph_class(info.codepoint) == hb.OTLayoutGlyphClass.MARK
        # A hidden default ignorable shows as the space glyph somewhere in its (possibly merged) cluster.
        members = code_points[info.cluster:bounds[bounds.index(info.cluster) + 1]]
        hidden = info.codepoint == space and 0x20 not in members and any(ignorable(c) for c in members)
        flags.append((0 if info.cluster in first else 2) | (1 if mark or hidden else 0))
        first.setdefault(info.cluster, index)
        marks.append(1 if mark else 0)
    starts = sorted(units[c] for c in first)
    glyph_of = {units[c]: g for c, g in first.items()}
    clusters = [glyph_of[max(s for s in starts if s <= u)] for u in range(unit)]
    placements = []
    for p in positions:
        placements += [bits(np.float32(p.x_advance) * scale), bits(np.float32(p.x_offset) * scale), bits(np.float32(p.y_offset) * scale)]
    return [info.codepoint for info in infos], placements, clusters, flags, marks


@functools.lru_cache(maxsize=None)
def directional(font_bytes):
    """Whether the font has a direction feature (ltra, ltrm, rtla, rtlm), which the engine's direction-independent
    plans do not apply."""
    font = TTFont(io.BytesIO(font_bytes))
    return any(record.FeatureTag in ('ltra', 'ltrm', 'rtla', 'rtlm') for tag in ('GSUB', 'GPOS') if tag in font
               and font[tag].table.FeatureList for record in font[tag].table.FeatureList.FeatureRecord)


def kept_ignorable(font_bytes, text, glyphs):
    """Whether HarfBuzz's output holds the nominal glyph of a default ignorable in the text: substitution touched it
    (a self-mapping, or a ligature taken apart again), so HarfBuzz keeps it visible, while the engine, seeing an
    unchanged glyph, hides it."""
    cmap = TTFont(io.BytesIO(font_bytes)).getBestCmap()
    order = TTFont(io.BytesIO(font_bytes)).getGlyphOrder()
    nominal = {order.index(cmap[ord(c)]) for c in text if ignorable(ord(c)) and ord(c) in cmap}
    return any(glyph in nominal for glyph in glyphs)


@functools.lru_cache(maxsize=None)
def miscounted_ligatures(font_bytes):
    """The ligature-coverage glyphs whose mark-to-ligature ComponentCount differs from the component count of the
    font's GSUB ligatures (of the glyph itself where GSUB forms it, of any ligature otherwise, as when a later
    substitution renames the ligature). A mark after such a ligature takes the table's last component in HarfBuzz
    and the ligature's last one in the engine."""
    font = TTFont(io.BytesIO(font_bytes))
    if 'GSUB' not in font or 'GPOS' not in font:
        return frozenset()
    order = font.getGlyphOrder()
    components = {}
    for lookup in font['GSUB'].table.LookupList.Lookup:
        for subtable in lookup.SubTable:
            for ligatures in (getattr(getattr(subtable, 'ExtSubTable', subtable), 'ligatures', None) or {}).values():
                for ligature in ligatures:
                    components[ligature.LigGlyph] = len(ligature.Component) + 1
    counts = set(components.values())
    miscounted = set()
    for lookup in font['GPOS'].table.LookupList.Lookup:
        for subtable in lookup.SubTable:
            subtable = getattr(subtable, 'ExtSubTable', subtable)
            if hasattr(subtable, 'LigatureArray'):
                for name, attach in zip(subtable.LigatureCoverage.glyphs, subtable.LigatureArray.LigatureAttach):
                    if attach.ComponentCount not in ({components[name]} if name in components else counts or {attach.ComponentCount}):
                        miscounted.add(order.index(name))
    return frozenset(miscounted)


# Known divergence classes of the emoji run corpus (MCEMOJI4): the case's output is the engine's.
EMOJI_DIVERGENCES = {1: 'direction features', 2: 'default ignorable kept by substitution', 3: 'miscounted ligature'}


def read_emoji_pins(path):
    """The engine outputs the emoji test printed for known divergent cases (pin_emoji lines), by (font, case)."""
    pins = {}
    if path:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith('pin_emoji '):
                fields = dict(item.split('=') for item in line.split()[1:])
                values = {name: [int(v) for v in fields[name].split(',')] if fields[name] else []
                          for name in ('glyphs', 'placements', 'map', 'flags', 'marks')}
                pins[int(fields['font']), int(fields['case'])] = values
    return pins


def emoji(pins_path=None):
    """emoji_run_fixture (MCEMOJI4; reads 3 or 4, maicord_native_emoji_runs): each case's glyphs, placements, map, flags
    and diacritics as HarfBuzz shapes it (see emoji_case); the fonts, weights and rejections are kept. A case may be
    known divergent (EMOJI_DIVERGENCES, one word after the case header) where its font has a direction feature (see
    directional), or HarfBuzz's output keeps a default ignorable's nominal glyph or a miscounted ligature (see
    kept_ignorable and miscounted_ligatures). Such a case is marked until pins are read; with --pins (the test's
    pin_emoji lines, MAICORD_FIXTURE_RECORD=1) it stays divergent, expecting the engine's output, only where its pin
    differs from HarfBuzz."""
    path = GENERATED / 'emoji_run_fixture.bin'
    data = path.read_bytes()
    assert data[:8] in (b'MCEMOJI3', b'MCEMOJI4')
    case_header = 20 if data[:8] == b'MCEMOJI4' else 16
    pins = read_emoji_pins(pins_path)
    at, out, counts = 12, bytearray(b'MCEMOJI4' + data[8:12]), [0] * (len(EMOJI_DIVERGENCES) + 1)
    for font_index in range(struct.unpack_from('<I', data, 8)[0]):
        embedded, size, case_count, reject, weight = struct.unpack_from('<5I', data, at)
        font_bytes = data[at + 20:at + 20 + size]
        at += 20 + size
        records = []
        for case_index in range(case_count):
            em_bits, length, n, units = struct.unpack_from('<4I', data, at)
            text = data[at + case_header:at + case_header + length].decode('utf-8')
            at += case_header + length + n * 2 + n * 12 + units * 4 + n * 2
            glyphs, placements, clusters, flags, marks = emoji_case(font_bytes, weight, text, em_bits)
            if directional(font_bytes):
                divergence = 1
            elif kept_ignorable(font_bytes, text, glyphs):
                divergence = 2
            elif miscounted_ligatures(font_bytes) & set(glyphs):
                divergence = 3
            else:
                divergence = 0
            if divergence and pins_path:
                pinned = pins.get((font_index, case_index))
                engine = tuple(pinned[name] for name in ('glyphs', 'placements', 'map', 'flags', 'marks')) if pinned else None
                if engine is None or engine == (list(glyphs), list(placements), list(clusters), list(flags), list(marks)):
                    divergence = 0
                else:
                    glyphs, placements, clusters, flags, marks = engine
            counts[divergence] += 1
            assert len(clusters) == units
            records.append(struct.pack('<5I', em_bits, length, len(glyphs), units, divergence) + text.encode('utf-8') +
                           struct.pack(f'<{len(glyphs)}H', *glyphs) + struct.pack(f'<{len(placements)}I', *placements) +
                           struct.pack(f'<{units}I', *clusters) + bytes(flags) + bytes(marks))
        out += struct.pack('<5I', embedded, size, len(records), reject, weight) + font_bytes + b''.join(records)
    assert at == len(data)
    write_embedded_array(GENERATED / 'emoji_run_fixture.inc', comment(), 'constexpr u8 emoji_fixture[]', bytes(out))
    print('emoji_run_fixture regenerated,', sum(counts), 'cases,',
          ', '.join(f'{counts[k]} known divergent ({v})' for k, v in EMOJI_DIVERGENCES.items()))


def layout_cases(data):
    """(glyph array offset, glyph count, text, em bits, glyphs, placement bits, map, font bytes, weight) of each case of an
    MCEMLIN1 or MCEMPAR1 corpus; a font that is not embedded is read from its recorded path."""
    paragraph = data[:8] == b'MCEMPAR1'
    assert paragraph or data[:8] == b'MCEMLIN1'
    at = 8

    def word():
        nonlocal at
        at += 4
        return struct.unpack_from('<I', data, at - 4)[0]

    for _ in range(word()):
        embedded, size, count, weight = word(), word(), word(), word()
        font = data[at:at + size]
        at += size
        font = font if embedded else Path(font.decode('utf-8')).read_bytes()
        for _ in range(count):
            em_bits, length, n, units, points, ranges = [word() for _ in range(6)]
            runs = word() if paragraph else 0
            if paragraph:
                word()
            text = data[at:at + length].decode('utf-8')
            at += length
            glyphs_at = at
            glyphs = list(struct.unpack_from(f'<{n}H', data, at))
            placements = list(struct.unpack_from(f'<{3 * n}I', data, at + 2 * n))
            sources = list(struct.unpack_from(f'<{units}I', data, at + 14 * n))
            at += 14 * n + 4 * units + 20
            clusters = word()
            at += 8 * clusters + (units + 1) * 2 * (13 if paragraph else 5) * 4
            if paragraph:
                at += 16 * points
                for _ in range(ranges):
                    word(), word()
                    rectangles = word()
                    at += 36 * rectangles
                if runs != 0xFFFFFFFF:
                    at += 28 * runs
            else:
                at += 16 * points + 28 * ranges
            yield glyphs_at, n, text, em_bits, glyphs, placements, sources, font, weight
    assert at == len(data)


# The characters a paragraph removes before shaping (native_text_nonvisual).
NONVISUAL = [(0, 31), (127, 159), (173, 173), (8203, 8203), (8206, 8207), (8232, 8238), (8288, 8292), (8294, 8303),
             (65279, 65279), (65529, 65531), (65534, 65535), (113824, 113827), (119155, 119162), (917505, 917505),
             (917536, 917631)]


def emoji_layout():
    """emoji_line_fixture (MCEMLIN1, maicord_native_emoji_lines) and emoji_paragraph_fixture (MCEMPAR1,
    maicord_native_emoji_paragraphs) keep their recorded line and paragraph geometry. Where HarfBuzz shapes a case (a
    paragraph's text without the nonvisual characters it removes) to the recorded placements and map but other glyph
    IDs (a hidden default ignorable shows HarfBuzz's space glyph), the case takes HarfBuzz's glyph IDs; elsewhere the
    glyph of a cluster of default ignorables becomes the space glyph. The geometry does not depend on glyph IDs."""
    for name in ('emoji_line_fixture', 'emoji_paragraph_fixture'):
        data = bytearray((GENERATED / f'{name}.bin').read_bytes())
        changed = 0
        for glyphs_at, n, text, em_bits, glyphs, placements, sources, font, weight in list(layout_cases(bytes(data))):
            units = []
            for c in text:
                visible = name == 'emoji_line_fixture' or not any(a <= ord(c) <= b for a, b in NONVISUAL)
                units += [visible] * (2 if ord(c) > 0xFFFF else 1)
            shown = ''.join(c for c in text if name == 'emoji_line_fixture' or not any(a <= ord(c) <= b for a, b in NONVISUAL))
            if not shown:
                continue
            shaped, shaped_placements, shaped_sources, _, _ = emoji_case(font, weight, shown, em_bits)
            if shaped != glyphs and shaped_placements == placements and shaped_sources == [s for s, v in zip(sources, units) if v]:
                struct.pack_into(f'<{n}H', data, glyphs_at, *shaped)
                changed += 1
                continue
            # Otherwise only the glyph of a cluster of default ignorables changes: HarfBuzz hides it as the space glyph.
            space = hb.Font(hb.Face(hb.Blob(font))).get_nominal_glyph(0x20) or 0
            owners = [s for c in text for s in [c] * (2 if ord(c) > 0xFFFF else 1)]
            hidden = [index for index in range(n) if glyphs[index] != space and
                      [c for c, s in zip(owners, sources) if s == index] and
                      all(ignorable(ord(c)) for c, s in zip(owners, sources) if s == index)]
            for index in hidden:
                struct.pack_into('<H', data, glyphs_at + 2 * index, space)
            changed += bool(hidden)
        declaration = 'constexpr u8 emoji_line_fixture[]' if name == 'emoji_line_fixture' else 'constexpr u8 emoji_paragraph_fixture[]'
        write_embedded_array(GENERATED / f'{name}.inc', f'// Recorded Windows geometry for first-party and system fonts; HarfBuzz {hb.version_string()} glyph IDs '
                             'where they alone differ (harfbuzz_fixture.py emoji_layout).\n', declaration, bytes(data))
        print(name, changed, 'cases take HarfBuzz glyph IDs')


DEFAULT_FEATURES = {'rvrn', 'rand', 'trak', 'abvm', 'blwm', 'ccmp', 'locl', 'mark', 'mkmk', 'rlig', 'calt', 'clig', 'curs',
                    'dist', 'kern', 'liga', 'rclt'}


def langsys(table):
    """The language system HarfBuzz selects for Latin in English: latn, then DFLT, dflt; ENG, then the default."""
    records = {record.ScriptTag: record.Script for record in table.ScriptList.ScriptRecord}
    for tag in ('latn', 'DFLT', 'dflt'):
        if tag in records:
            for record in records[tag].LangSysRecord:
                if record.LangSysTag == 'ENG ':
                    return record.LangSys
            return records[tag].DefaultLangSys
    return None


def lookup_glyphs(lookup):
    """The glyph names a lookup's subtables can act on, as sfnt_nominal_participation_add counts them: coverages
    (the first of a context, every input coverage of a format-3 context) and ligature components; reverse chaining
    substitution adds none."""
    names = set()
    for subtable in lookup.SubTable:
        subtable = getattr(subtable, 'ExtSubTable', subtable)
        if hasattr(subtable, 'Substitute'):
            continue
        # The first coverage of the subtable (a mark attachment's mark coverage).
        coverage = getattr(subtable, 'Coverage', None) or getattr(subtable, 'MarkCoverage', None) or getattr(subtable, 'Mark1Coverage', None)
        for table in coverage if isinstance(coverage, list) else [coverage] if coverage else []:
            names |= set(table.glyphs)
        for table in getattr(subtable, 'InputCoverage', None) or []:
            names |= set(table.glyphs)
        for attribute in ('mapping', 'ligatures', 'alternates'):
            value = getattr(subtable, attribute, None)
            if value:
                names |= set(value)
                if attribute == 'ligatures':
                    names |= {c for ligatures in value.values() for ligature in ligatures for c in ligature.Component}
    return names


def participating(table):
    """The glyph names the default features (any language system) or a required feature can act on."""
    features = table.FeatureList.FeatureRecord if table.FeatureList else []
    indices = {i for i, record in enumerate(features) if record.FeatureTag in DEFAULT_FEATURES}
    for record in table.ScriptList.ScriptRecord if table.ScriptList else []:
        for system in [record.Script.DefaultLangSys] + [r.LangSys for r in record.Script.LangSysRecord]:
            if system is not None and system.ReqFeatureIndex != 0xFFFF:
                indices.add(system.ReqFeatureIndex)
    return {name for i in indices for lookup in features[i].Feature.LookupListIndex
            for name in lookup_glyphs(table.LookupList.Lookup[lookup])}


def participation():
    """nominal_participation_fixture (MCNOM01, maicord_native_nominal_participation): per font, the glyphs below its
    limit (glyph zero excluded) that HarfBuzz's default features or a required feature can act on (see participating)."""
    data = (GENERATED / 'nominal_participation_fixture.bin').read_bytes()
    assert data[:8] == b'MCNOM01\0'
    at, out = 12, bytearray(data[:12])
    for _ in range(struct.unpack_from('<I', data, 8)[0]):
        size, kind, limit = struct.unpack_from('<3I', data, at)
        raw = data[at + 20:at + 20 + size]
        font = TTFont(io.BytesIO(raw))
        order = font.getGlyphOrder()
        bitmap = 0
        for name in participating(font['GPOS' if kind else 'GSUB'].table):
            glyph = order.index(name)
            if 0 < glyph < limit:
                bitmap |= 1 << glyph
        out += struct.pack('<3IQ', size, kind, limit, bitmap) + raw
        at += 20 + size
    assert at == len(data)
    write_embedded_array(GENERATED / 'nominal_participation_fixture.inc', comment(), 'constexpr u8 participation_fixture[]', bytes(out))
    print('nominal_participation_fixture regenerated')


# Known divergence classes of the lookup plan corpus (MCPLN04): the config's expectations are the engine's.
PLAN_DIVERGENCES = {1: 'direction features'}
DIRECTION_OFF = {'ltra': False, 'ltrm': False, 'rtla': False, 'rtlm': False}


def plan():
    """lookup_plan_fixture (MCPLN04, maicord_native_lookup_plan): per font and typography mode, the lookups of
    HarfBuzz's default features in the selected language system (see langsys; independent letters drop liga, clig
    and kern), the glyph and placement HarfBuzz gives 'a', whether 'a' keeps its nominal glyph (no default or required
    feature of any language system covers it) and the application's result. The selected script and language are
    kept. A font with a direction feature, which the engine's direction-independent plan leaves out, is known
    divergent (class 1, PLAN_DIVERGENCES): its glyph and placement are HarfBuzz's without the direction features, as
    the engine shapes it. Reads MCPLN03 (the recorded corpus) or MCPLN04."""
    data = (GENERATED / 'lookup_plan_fixture.bin').read_bytes()
    assert data[:8] in (b'MCPLN03\0', b'MCPLN04\0')
    config = 48 if data[:8] == b'MCPLN03\0' else 52
    at, out, divergent = 12, bytearray(), 0
    count = struct.unpack_from('<I', data, 8)[0]
    for _ in range(count):
        size, kind = struct.unpack_from('<2I', data, at)
        header, raw = data[at:at + 20], data[at + 20:at + 20 + size]
        at += 20 + size
        for _ in range(2):
            at += config + 2 * struct.unpack_from('<I', data, at + config - 40)[0]
        divergence = 1 if directional(raw) else 0
        divergent += 2 if divergence else 0
        font = TTFont(io.BytesIO(raw))
        table = font['GPOS' if kind else 'GSUB'].table
        features = table.FeatureList.FeatureRecord
        covered = participating(table)
        nominal_name = font.getBestCmap().get(ord('a'))
        nominal = font.getGlyphID(nominal_name) if nominal_name else 0
        simple = int(bool(nominal) and nominal_name not in covered)
        scale = np.float32(1000) / np.float32(font['head'].unitsPerEm)
        system = langsys(table)
        out += header + raw
        for independent in (0, 1):
            # HarfBuzz pauses after rvrn; a tag uses its first listed feature (hb_ot_layout_language_find_feature); the
            # required feature runs in its tag's stage, else the first.
            stages, seen = [set(), set()], set()
            if system is not None:
                enabled = lambda tag: tag in DEFAULT_FEATURES and not (independent and tag in INDEPENDENT)
                for i in system.FeatureIndex:
                    tag = features[i].FeatureTag
                    if enabled(tag) and tag not in seen and i != system.ReqFeatureIndex:
                        stages[tag != 'rvrn'] |= set(features[i].Feature.LookupListIndex)
                    seen.add(tag)
                if system.ReqFeatureIndex != 0xFFFF:
                    tag = features[system.ReqFeatureIndex].FeatureTag
                    stages[enabled(tag) and tag != 'rvrn'] |= set(features[system.ReqFeatureIndex].Feature.LookupListIndex)
            buffer = hb.Buffer()
            buffer.add_str('a')
            buffer.direction, buffer.script, buffer.language = 'ltr', 'Latn', 'en'
            requested = {**(INDEPENDENT if independent else {}), **(DIRECTION_OFF if divergence else {})}
            hb.shape(hb.Font(hb.Face(hb.Blob(raw))), buffer, requested)
            glyph, p = buffer.glyph_infos[0].codepoint, buffer.glyph_positions[0]
            placement = [bits(np.float32(p.x_advance) * scale), bits(np.float32(p.x_offset) * scale), bits(np.float32(p.y_offset) * scale)]
            if simple and not independent:
                app_glyph, app_placement = nominal, [bits(np.float32(font['hmtx'][nominal_name][0]) * scale), 0, 0]
            else:
                app_glyph, app_placement = glyph, placement
            ordered = sorted(stages[0]) + sorted(stages[1])
            out += struct.pack('<5I', independent, 0, divergence, len(ordered), glyph) + struct.pack('<3I', *placement)
            out += struct.pack('<2I', simple, app_glyph) + struct.pack('<3I', *app_placement)
            out += struct.pack(f'<{len(ordered)}H', *ordered)
    assert at == len(data)
    write_embedded_array(GENERATED / 'lookup_plan_fixture.inc', comment(), 'constexpr u8 plan_fixture[]',
                         b'MCPLN04\0' + struct.pack('<I', count) + bytes(out))
    print('lookup_plan_fixture', count, 'fonts,', divergent, 'known divergent configurations (direction features)')


@functools.lru_cache(maxsize=None)
def multiplies(font_bytes):
    """Whether the font's GSUB has a multiple substitution."""
    font = TTFont(io.BytesIO(font_bytes))
    return 'GSUB' in font and any(lookup_type(lookup) == 2 for lookup in font['GSUB'].table.LookupList.Lookup)


# Known divergence classes of the Hebrew compatibility cases: the case's output is the engine's.
HEBREW_DIVERGENCES = {1: 'left-to-right Hebrew (HarfBuzz reverses its graphemes)', 2: 'GPOS meets a default ignorable other than a joiner',
                      3: 'GPOS meets a multiple substitution output'}


def hebrew(pins_path=None):
    """hebrew_compat_fixture.inc (maicord_native_latin_guards): each case shaped as Hebrew at 17.5 px in its direction
    (mode bit 4: right to left), a right-to-left one beginning its paragraph, with the independent-letter features off
    when mode bit 1 is set; the fonts are kept. A case may be known divergent (HEBREW_DIVERGENCES, the last field of
    its row) where it is left to right (HarfBuzz reverses a left-to-right Hebrew run's graphemes,
    hb_ensure_native_direction, which the engine does only for Arabic), where its font's GPOS meets a default ignorable
    other than ZWJ and ZWNJ, which HarfBuzz's positioning skips and the engine's does not, or a multiple substitution's
    output, where HarfBuzz and the engine choose different attachment targets. Such a case is marked until pins are
    read; with --pins (the test's pin_hebrew lines, MAICORD_FIXTURE_RECORD=1) it stays divergent, expecting the
    engine's output, only where its pin differs from HarfBuzz."""
    path = GENERATED / 'hebrew_compat_fixture.inc'
    text = path.read_text(encoding='utf-8')
    fonts = [(GENERATED / 'hebrew_compat_fixture' / f'hebrew_compat_font_{i}.bin').read_bytes()
             for i in range(len(re.findall(r'\{hebrew_compat_font_\d+,sizeof', text)))]
    em_bits = struct.unpack('<I', struct.pack('<f', 17.5))[0]
    cases = re.findall(r'\{(\d+),(\d+),\{"((?:[^"\\]|\\.)*)",\d+\},\{hebrew_glyphs_\d+', text)
    head = text[:text.index('constexpr u16 hebrew_glyphs_0[]')]
    head = re.sub(r'\A(//[^\n]*\n)+', '', head)
    pins = read_shaped_pins(pins_path, 'hebrew')
    arrays, rows, counts = [], [], [0] * (len(HEBREW_DIVERGENCES) + 1)
    for index, (font, mode, literal) in enumerate(cases):
        encoded = codecs.escape_decode(literal.encode('latin-1'))[0]
        scalars = [ord(c) for c in encoded.decode('utf-8')]
        assert all(c <= 0xFFFF for c in scalars)
        rtl = bool(int(mode) & 4)
        positions = font_traits(fonts[int(font)])[0]
        if not rtl:
            divergence = 1
        elif positions and any(ignorable(s) and s not in (0x200C, 0x200D) for s in scalars):
            divergence = 2
        elif positions and multiplies(fonts[int(font)]):
            divergence = 3
        else:
            divergence = 0
        glyphs, clusters, placements = shape(fonts[int(font)], scalars, rtl, em_bits, int(mode) & 1, script='hebr', bot=rtl)
        # With pins, a case stays divergent only where the engine's pin differs (one without a pin was checked).
        if divergence and pins_path:
            pinned = pins.get((0, index))
            if pinned is None or (pinned['glyphs'], pinned['clusters'], pinned['placements']) == (list(glyphs), list(clusters), list(placements)):
                divergence = 0
            else:
                glyphs, clusters, placements = pinned['glyphs'], pinned['clusters'], pinned['placements']
        counts[divergence] += 1
        arrays.append(f'constexpr u16 hebrew_glyphs_{index}[]{{\n    ' + ','.join(map(str, glyphs)) + ',\n};\n'
                      f'constexpr u32 hebrew_clusters_{index}[]{{\n    ' + ','.join(map(str, clusters)) + ',\n};\n'
                      f'constexpr u32 hebrew_placements_{index}[]{{\n    ' + ','.join(map(str, placements)) + ',\n};\n')
        rows.append(f'    {{{font},{mode},{{"{literal}",{len(encoded)}}},{{hebrew_glyphs_{index},{len(glyphs)}}},'
                    f'{{hebrew_clusters_{index},{len(clusters)}}},{{hebrew_placements_{index},{len(placements)}}},{divergence}}},\n')
    path.write_text(comment() + head + ''.join(arrays) +
                    'struct HebrewCompatibilityCase {u32 font,mode;Utf8View text;ConstSlice<u16> glyphs;ConstSlice<u32> clusters,placements;u32 divergence;};\n'
                    'constexpr HebrewCompatibilityCase hebrew_compat_cases[]{\n' + ''.join(rows) + '};\n', encoding='utf-8')
    print('hebrew_compat_fixture', len(rows), 'cases,', ', '.join(f'{counts[k]} known divergent ({v})' for k, v in HEBREW_DIVERGENCES.items()))


FAMILIES = {'arabic': arabic, 'component': component, 'emoji': emoji, 'emoji_layout': emoji_layout, 'gpos': gpos,
            'hebrew': hebrew, 'joining': joining, 'marks': marks, 'participation': participation, 'plan': plan}


# Families that keep known divergent cases take the engine's values from a test's recorded pin lines.
PINNED = {'arabic', 'component', 'emoji', 'gpos', 'hebrew'}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('family', choices=sorted(FAMILIES))
    parser.add_argument('--pins', help='test output recorded with MAICORD_FIXTURE_RECORD=1 (pin_* lines)')
    arguments = parser.parse_args()
    if arguments.family in PINNED:
        FAMILIES[arguments.family](arguments.pins)
    else:
        FAMILIES[arguments.family]()


if __name__ == '__main__':
    sys.exit(main())
