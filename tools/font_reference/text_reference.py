"""Records the committed references of MaiCordNativeAppLineTests (tests/ui/text/app_line_reference.hpp).

    python tools/font_reference/text_reference.py record [--build D:/MaiCord-build/Release] [--merge] [--report FILE]
    python tools/font_reference/text_reference.py report [--report FILE]
    python tools/font_reference/text_reference.py explain KEY

`record` runs every registered MaiCordNativeAppLineTests test (from `ctest --show-only=json-v1` in the build directory)
with MAICORD_TEXT_REFERENCE_RECORD set, so each process writes its layout digests and its shaping requests, shapes
every request with HarfBuzz (uharfbuzz) and writes tests/generated/app_line_reference.{bin,inc}. The layout digests are
the engine's own output; the shaping digests are HarfBuzz's. The comparison is in logical order and at the engine's
cluster granularity (it keeps one cluster per syllable, where HarfBuzz may merge or split), with glyph IDs and
positions exact. A run whose native shape still differs from HarfBuzz's is marked divergent: the test counts it but
does not fail, and `report` lists divergent runs per script, raw and normalised, and the normalised ones by kind.

Each group of 64 cases records the fonts it used. A machine whose system fonts differ skips only the groups and runs
that used them (repository fonts must match everywhere); `record` prints how many groups such a machine still checks.
`--merge` keeps the committed profiles and adds this machine's fonts and group digests, so a second machine (a CI
runner) checks everything too; record it from the same source revision. Without it the reference is rebuilt from this
machine alone. `explain` prints the native and HarfBuzz shapes of one run key (from a test's failure message) out of the
last recording.

Requires uharfbuzz (HarfBuzz is the shaping reference) and runs from the repository root.
"""
import argparse
import collections
import concurrent.futures
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import uharfbuzz as hb

sys.path.insert(0, str(Path(__file__).resolve().parent))
from embed_fixture import write_embedded_array  # noqa: E402

TARGET = 'MaiCordNativeAppLineTests'
OUTPUT = Path('tests/generated/app_line_reference.inc')
RECORDS = Path(tempfile.gettempdir()) / 'maicord_text_reference'
SEED = 14695981039346656037
PRIME = 1099511628211
MASK = (1 << 64) - 1
SEPARATORS = {0x0D, 0x0A, 0x0B, 0x0C, 0x85, 0x2028, 0x2029}
# Scripts HarfBuzz shapes right to left as given (hb_script_get_horizontal_direction: right-to-left scripts, and the
# directionless Common, Inherited and Unknown); any other script forced right to left is shaped by graphemes.
RTL_SCRIPTS = {'Arab', 'Hebr', 'Syrc', 'Thaa', 'Cprt', 'Khar', 'Phnx', 'Nkoo', 'Lydi', 'Avst', 'Armi', 'Phli', 'Prti',
               'Sarb', 'Orkh', 'Samr', 'Mand', 'Mend', 'Nbat', 'Narb', 'Palm', 'Phlp', 'Hatr', 'Hung', 'Adlm', 'Rohg',
               'Sogo', 'Sogd', 'Elym', 'Chrs', 'Yezi', 'Ougr', 'Zyyy', 'Zinh', 'Zzzz', 'None'}


def fnv(data, h=SEED):
    """hash64_bytes of include/maicord/core/hash.hpp."""
    for b in data:
        h = ((h ^ b) * PRIME) & MASK
    return h


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, fmt):
        values = struct.unpack_from('<' + fmt, self.data, self.at)
        self.at += struct.calcsize('<' + fmt)
        return values if len(values) > 1 else values[0]

    def units(self, count):
        values = struct.unpack_from(f'<{count}H', self.data, self.at)
        self.at += 2 * count
        return values


def read_record(path):
    """One process's record (app_line_reference.cpp, reference_end)."""
    r = Reader(path.read_bytes())
    if r.data[:8] != b'MCTXREC2':
        raise ValueError(f'{path}: not a text reference record')
    r.at = 8
    profile = r.take('Q')
    profile_units, cases, font_count, group_count = r.take('4I')
    text = bytes(r.data[r.at:r.at + 2 * profile_units]).decode('utf-16-le')
    r.at += 2 * profile_units
    fonts = []
    for _ in range(font_count):
        digest, face, upem, system, path_units = r.take('QIHHH')
        font_path = bytes(r.data[r.at:r.at + 2 * path_units]).decode('utf-16-le')
        r.at += 2 * path_units
        if font_path.startswith('\\\\?\\'):
            font_path = font_path[4:]
        fonts.append({'digest': digest, 'face': face, 'upem': upem, 'system': bool(system), 'path': font_path})
    groups = [r.take('IQQ') for _ in range(group_count)]  # (index, digest, mask of the fonts used)
    requests = []
    for _ in range(r.take('I')):
        key, font, flags, measured, axis_count = r.take('QHBBB')
        axes = [r.take('If') for _ in range(axis_count)]
        anchor = r.take('I')
        before, run, after = r.take('3H')
        units = r.units(before + run + after)
        em = r.take('f')
        glyphs = [r.take('H3f') for _ in range(r.take('H'))]
        clusters = r.units(run)
        requests.append({'key': key, 'font': fonts[font], 'flags': flags, 'measured': measured, 'axes': axes,
                         'anchor': anchor, 'before': before, 'run': run, 'after': after, 'units': units, 'em': em,
                         'glyphs': glyphs, 'clusters': clusters})
    return {'profile': profile, 'text': text, 'cases': cases, 'fonts': fonts, 'groups': groups, 'requests': requests}


def font_name(path):
    name = path.replace('/', '\\').rsplit('\\', 1)[-1]
    return ''.join(c.lower() if c < '\x80' else '?' for c in name)[:63]


# HarfBuzz ------------------------------------------------------------------------------------------------------------

_faces = {}


def hb_font(font, axes):
    key = (font['path'], font['face'])
    if key not in _faces:
        _faces[key] = hb.Face(hb.Blob.from_file_path(font['path']), font['face'])
    result = hb.Font(_faces[key])
    if axes:
        result.set_variations({struct.pack('>I', tag).decode('latin-1'): value for tag, value in axes})
    return result


def scalars(units):
    """(code points, UTF-16 offset of each)."""
    out, offsets, i = [], [], 0
    while i < len(units):
        c = units[i]
        offsets.append(i)
        if 0xD800 <= c < 0xDC00 and i + 1 < len(units) and 0xDC00 <= units[i + 1] < 0xE000:
            out.append(0x10000 + ((c - 0xD800) << 10) + (units[i + 1] - 0xDC00))
            i += 2
        else:
            out.append(c)
            i += 1
    return out, offsets


def script_of(code_point):
    probe = hb.Buffer()
    probe.add_codepoints([code_point])
    probe.guess_segment_properties()
    return probe.script


def shape(request):
    """HarfBuzz's glyphs of the run in logical order: (gid, cluster unit within the run, advance, x, y)."""
    cps, offsets = scalars(request['units'])
    first = sum(1 for o in offsets if o < request['before'])
    count = sum(1 for o in offsets if request['before'] <= o < request['before'] + request['run'])
    buffer = hb.Buffer()
    buffer.add_codepoints(cps, first, count)
    buffer.direction = 'rtl' if request['flags'] & 1 else 'ltr'
    buffer.language = 'en'
    # The run begins or ends its paragraph (bits 4 and 8): HarfBuzz may then insert a dotted circle before a leading mark.
    buffer.flags = (hb.BufferFlags.BOT if request['flags'] & 4 else 0) | (hb.BufferFlags.EOT if request['flags'] & 8 else 0)
    if request['anchor']:
        buffer.script = script_of(request['anchor'])
    buffer.guess_segment_properties()
    features = {'liga': False, 'clig': False, 'kern': False} if request['flags'] & 2 else {}
    hb.shape(hb_font(request['font'], request['axes']), buffer, features)
    glyphs = [(info.codepoint, offsets[info.cluster] - request['before'], pos.x_advance, pos.x_offset, pos.y_offset)
              for info, pos in zip(buffer.glyph_infos, buffer.glyph_positions)]
    if request['flags'] & 1:
        if str(buffer.script) in RTL_SCRIPTS:
            glyphs.reverse()
        else:
            # A left-to-right script forced right to left: HarfBuzz reverses its graphemes and shapes them left to right
            # (hb_ensure_native_direction), so only the clusters are in visual order; each keeps its glyphs' order.
            runs = []
            for glyph in glyphs:
                if runs and runs[-1][0][1] == glyph[1]:
                    runs[-1].append(glyph)
                else:
                    runs.append([glyph])
            glyphs = [glyph for run in reversed(runs) for glyph in run]
    return glyphs, buffer.script


# Canonical forms (canonical_run in app_line_reference.cpp) ------------------------------------------------------------

def digest(groups, glyphs):
    """(structure, positions) 64-bit digests of groups (first unit, unit count, [glyph indices]) over glyphs (gid,
    advance, x, y)."""
    s = p = SEED
    for first, count, members in groups:
        s = fnv(struct.pack('<3H', first, count, len(members)), s)
        for g in members:
            gid, advance, x, y = glyphs[g]
            s = fnv(struct.pack('<H', gid), s)
            p = fnv(struct.pack('<3i', advance, x, y), p)
    return s, p


def folded(structure, positions, measured):
    """The run's 32-bit reference digest: the structure's, extended by the positions' unless measured."""
    value = structure if measured else fnv(struct.pack('<Q', positions), structure)
    return (value ^ value >> 32) & 0xFFFFFFFF


def native_canonical(request):
    scale = request['font']['upem'] / request['em']
    rtl = request['flags'] & 1

    def design(value):
        scaled = value * scale
        return int(scaled - .5) if scaled < 0 else int(scaled + .5)

    glyphs = [(gid, design(a), -design(x) if rtl else design(x), design(y)) for gid, a, x, y in request['glyphs']]
    clusters, total, groups, u = request['clusters'], len(request['glyphs']), [], 0
    while u < len(clusters):
        value, w = clusters[u], u + 1
        while w < len(clusters) and clusters[w] == value:
            w += 1
        members = []
        if value != 0xFFFF:
            stop = total
            for later in clusters[w:]:
                if later != 0xFFFF:
                    if later > value:
                        stop = later
                    break
            members = list(range(value, stop))
        groups.append((u, w - u, members))
        u = w
    return groups, glyphs


def harfbuzz_canonical(request, shaped):
    run_units = request['units'][request['before']:request['before'] + request['run']]
    starts = sorted({cluster for _, cluster, *_ in shaped})
    members = collections.defaultdict(list)
    for index, (_, cluster, *_) in enumerate(shaped):
        members[cluster].append(index)
    value = [None] * len(run_units)
    for index, start in enumerate(starts):
        stop = starts[index + 1] if index + 1 < len(starts) else len(run_units)
        # Paragraph separators end the shaped text: a cluster of only separators has no glyphs, like the engine's.
        mark = -1 if all(c in SEPARATORS for c in run_units[start:stop]) else start
        for u in range(max(start, 0), min(stop, len(run_units))):
            value[u] = mark
    groups, u = [], 0
    while u < len(value):
        w = u + 1
        while w < len(value) and value[w] == value[u]:
            w += 1
        groups.append((u, w - u, members[value[u]] if value[u] not in (None, -1) else []))
        u = w
    return groups, [(gid, a, x, y) for gid, _, a, x, y in shaped]


def visual_x(glyphs, order):
    """Each glyph's x from the left edge of the cluster when drawn in `order` (left to right): its pen plus offset."""
    x, pen = {}, 0
    for g in order:
        x[g] = pen + glyphs[g][2]
        pen += glyphs[g][1]
    return x


def normalised_equal(native, harfbuzz, measured, rtl, by_grapheme):
    """Whether the native shape equals HarfBuzz's at the engine's cluster granularity: every engine cluster boundary is
    a HarfBuzz cluster boundary (an engine cluster that HarfBuzz splits across its edge differs), and the HarfBuzz
    clusters inside each engine cluster carry exactly its glyph IDs, in logical order, with the same advances and
    vertical offsets and either the same horizontal offsets or the same horizontal places in the drawn cluster. The
    engine keeps one cluster per syllable for its caret contract; HarfBuzz may merge or split within one. Places matter
    for right-to-left runs: the engine measures offsets from the reading-direction pen while HarfBuzz lays out left to
    right, and shapes a left-to-right script forced right to left by graphemes (`by_grapheme`), each left to right."""
    (native_groups, native_glyphs), (hb_groups, hb_glyphs) = native, harfbuzz
    hb_at = {first: index for index, (first, _, _) in enumerate(hb_groups)}
    hb_end = sum(count for _, count, _ in hb_groups)
    for first, count, members in native_groups:
        if first not in hb_at or (first + count != hb_end and first + count not in hb_at):
            return False
        clusters, index = [], hb_at[first]
        while index < len(hb_groups) and hb_groups[index][0] < first + count:
            clusters.append(hb_groups[index][2])
            index += 1
        merged = [g for cluster in clusters for g in cluster]
        if [native_glyphs[g][0] for g in members] != [hb_glyphs[g][0] for g in merged]:
            return False
        if measured or [native_glyphs[g] for g in members] == [hb_glyphs[g] for g in merged]:
            continue
        # Otherwise the places must agree. The engine's right-to-left run draws a glyph at the cluster's right edge less
        # its pen, advance and offset; its canonical offset is already negated, so the place from the left edge is what
        # follows it plus that.
        if rtl:
            total = sum(native_glyphs[g][1] for g in members)
            pen, mine = 0, {}
            for g in members:
                mine[g] = total - pen - native_glyphs[g][1] + native_glyphs[g][2]
                pen += native_glyphs[g][1]
            order = [g for cluster in reversed(clusters) for g in cluster] if by_grapheme else list(reversed(merged))
        else:
            mine, order = visual_x(native_glyphs, members), merged
        theirs = visual_x(hb_glyphs, order)
        for a, b in zip(members, merged):
            if (native_glyphs[a][1], native_glyphs[a][3], mine[a]) != (hb_glyphs[b][1], hb_glyphs[b][3], theirs[b]):
                return False
    return True


def compare(request):
    """(reference digest, raw kind, normalised kind, script) over every native shape recorded for the request
    (`instances`: the same HarfBuzz input may meet different native output). A kind is None when the shapes are
    equal. HarfBuzz's output is already in logical order (shape reverses right-to-left buffers). A run equal only
    at the engine's cluster granularity is conformant; its reference is the native digest it was found equal with."""
    shaped, script = shape(request)
    harfbuzz = harfbuzz_canonical(request, shaped)
    reference = digest(*harfbuzz)
    raw = normalised = None
    native_digests = set()
    for instance in request['instances']:
        canonical = native_canonical(instance)
        native = digest(*canonical)
        native_digests.add(folded(*native, request['measured']))
        if native[0] != reference[0]:
            raw = 'glyphs or clusters'
        elif not request['measured'] and native[1] != reference[1] and not raw:
            raw = 'positions'
        by_grapheme = bool(request['flags'] & 1) and str(script) not in RTL_SCRIPTS
        if not normalised_equal(canonical, harfbuzz, request['measured'], bool(request['flags'] & 1), by_grapheme):
            same_glyphs = [g[0] for g in canonical[1]] == [g[0] for g in harfbuzz[1]]
            normalised = normalised or ('positions' if same_glyphs and not request['measured'] else 'glyphs or clusters')
    if raw and not normalised and len(native_digests) == 1:
        return native_digests.pop(), raw, None, script
    if raw and not normalised:
        normalised = 'native shapes differ'
    return folded(*reference, request['measured']), raw, normalised, script


# Running the tests ---------------------------------------------------------------------------------------------------

def registered_tests(build):
    listing = subprocess.run(['ctest', '--show-only=json-v1'], cwd=build, capture_output=True, text=True, check=True)
    tests = []
    for test in json.loads(listing.stdout)['tests']:
        command = test.get('command') or []
        if not command or TARGET not in Path(command[0]).name:
            continue
        properties = {p['name']: p['value'] for p in test.get('properties', [])}
        tests.append((test['name'], command, properties.get('ENVIRONMENT', []), properties.get('WORKING_DIRECTORY', build)))
    return tests


def run_tests(build, jobs):
    RECORDS.mkdir(parents=True, exist_ok=True)
    for stale in RECORDS.glob('*.bin'):
        stale.unlink()
    profiles = RECORDS / 'profiles'
    shutil.rmtree(profiles, ignore_errors=True)

    def run(indexed):
        index, (name, command, environment, directory) = indexed
        env = dict(os.environ, MAICORD_TEXT_REFERENCE_RECORD=str(RECORDS))
        # The registered TEMP/TMP/LOCALAPPDATA name a profile tools/build.ps1 deletes after its run; each test gets
        # its own here instead (an empty value, which some tests register, stays empty).
        scratch = profiles / str(index)
        for item in environment:
            key, _, value = item.partition('=')
            if value and key in ('TEMP', 'TMP', 'LOCALAPPDATA'):
                value = str(scratch / ('Local' if key == 'LOCALAPPDATA' else 'Temp'))
                Path(value).mkdir(parents=True, exist_ok=True)
            env[key] = value
        done = subprocess.run(command, cwd=directory, env=env, capture_output=True)
        return name, done.returncode, done.stdout.decode('utf-8', 'replace')

    tests = registered_tests(build)
    if not tests:
        raise SystemExit(f'no {TARGET} tests registered in {build}')
    failed = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for name, status, output in pool.map(run, enumerate(tests)):
            if status:
                failed.append((name, output.strip().splitlines()[-6:]))
    shutil.rmtree(profiles, ignore_errors=True)
    for name, lines in failed:
        print(f'{name} failed while recording:', *lines, sep='\n  ')
    if failed:
        raise SystemExit('a failing test records nothing trustworthy; fix it first')
    print(f'recorded {len(tests)} tests')


# The reference -------------------------------------------------------------------------------------------------------

def empty_reference():
    return {'fonts': [], 'profiles': {}, 'runs': {}, 'divergent': set()}


def read_reference(path):
    """The committed reference (app_line_reference.cpp, select_profile): fonts as (digest, face, system, name), profiles
    by key with their cases, font indices and (group index, digest) pairs, runs and divergent run keys."""
    data = path.read_bytes() if path.is_file() else b''
    reference = empty_reference()
    if data[:8] != b'MCTXREF2':
        return reference
    r = Reader(data)
    r.at = 8
    font_count, profile_count, run_count, _ = r.take('4I')
    for _ in range(font_count):
        digest_, face, flags = r.take('QII')
        reference['fonts'].append((digest_, face, bool(flags & 1), bytes(r.data[r.at:r.at + 64]).rstrip(b'\0').decode('latin-1')))
        r.at += 64
    for _ in range(profile_count):
        key, cases, fonts, groups, _ = r.take('Q4I')
        font_ids = {r.take('I') for _ in range(fonts)}
        reference['profiles'][key] = {'cases': cases, 'fonts': font_ids,
                                      'groups': {(lambda g: (g[0], g[2]))(r.take('IIQ')) for _ in range(groups)}}
    keys = []
    for _ in range(run_count):
        key, value = r.take('QI')
        reference['runs'][key] = value
        keys.append(key)
    bits = data[r.at:]
    reference['divergent'] = {key for index, key in enumerate(keys) if bits[index >> 3] >> (index & 7) & 1}
    return reference


def write_reference(reference):
    out = bytearray(b'MCTXREF2')
    out += struct.pack('<4I', len(reference['fonts']), len(reference['profiles']), len(reference['runs']), 0)
    for digest_, face, system, name in reference['fonts']:
        out += struct.pack('<QII', digest_, face, 1 if system else 0) + name.encode('latin-1')[:63].ljust(64, b'\0')
    for key in sorted(reference['profiles']):
        profile = reference['profiles'][key]
        out += struct.pack('<Q4I', key, profile['cases'], len(profile['fonts']), len(profile['groups']), 0)
        out += b''.join(struct.pack('<I', i) for i in sorted(profile['fonts']))
        out += b''.join(struct.pack('<IIQ', index, 0, value) for index, value in sorted(profile['groups']))
    keys = sorted(reference['runs'])
    for key in keys:
        out += struct.pack('<QI', key, reference['runs'][key])
    bits = bytearray((len(keys) + 7) // 8)
    for index, key in enumerate(keys):
        if key in reference['divergent']:
            bits[index >> 3] |= 1 << (index & 7)
    out += bits
    write_embedded_array(OUTPUT, '// Generated by tools/font_reference/text_reference.py: layout digests and HarfBuzz '
                         'run digests of MaiCordNativeAppLineTests.\n', 'constexpr u8 app_line_reference[]', out)
    return len(out)


def add_records(reference, records):
    """Adds this machine's profiles: their fonts, cases and group digests (an index may keep one digest per machine)."""
    index = {(f[0], f[1]): i for i, f in enumerate(reference['fonts'])}
    for record_ in records:
        ids = []
        for f in record_['fonts']:
            key = (f['digest'], f['face'])
            if key not in index:
                index[key] = len(reference['fonts'])
                reference['fonts'].append((f['digest'], f['face'], f['system'], font_name(f['path'])))
            ids.append(index[key])
        profile = reference['profiles'].setdefault(record_['profile'], {'cases': record_['cases'], 'fonts': set(), 'groups': set()})
        if profile['cases'] != record_['cases']:
            print(f'warning: {record_["text"]!r} ran {record_["cases"]} cases here, {profile["cases"]} where recorded before')
            profile['cases'] = record_['cases']
        profile['fonts'].update(ids)
        profile['groups'].update((group, value) for group, value, _ in record_['groups'])


def coverage(records):
    """What a machine with other system fonts still checks: groups that used only the repository's fonts."""
    groups = portable = 0
    whole = partial = none = 0
    for record_ in records:
        system = sum(1 << i for i, f in enumerate(record_['fonts']) if f['system'])
        kept = sum(1 for _, _, mask in record_['groups'] if not mask & system)
        groups += len(record_['groups'])
        portable += kept
        if kept == len(record_['groups']):
            whole += 1
        elif kept:
            partial += 1
        else:
            none += 1
    print(f'with other system fonts: {portable} of {groups} groups still checked; {whole} tests check everything, '
          f'{partial} part, {none} skip (status 77)')


def recorded_requests():
    records = [read_record(path) for path in sorted(RECORDS.glob('*.bin'))]
    if not records:
        raise SystemExit(f'no records in {RECORDS}; run `record` first')
    requests = {}
    for record in records:
        for request in record['requests']:
            request['profile'] = record['text']
            requests.setdefault(request['key'], dict(request, instances=[]))['instances'].append(request)
    return records, requests


def describe(request):
    units = request['units'][request['before']:request['before'] + request['run']]
    return bytes(struct.pack(f'<{len(units)}H', *units)).decode('utf-16-le', 'replace')


# Runs the reference itself makes divergent, by key, with the reason; `report` lists them apart from the engine's.
REFERENCE_CAUSED = {
    # ZillaSlab 'V' at the end of a line whose trailing space follows it: the engine shapes the line as HarfBuzz would
    # ('V ' kerns V to 614 units) and leaves the emergency break before 'V' alone (the cut there crosses nothing);
    # the reference shapes the published run 'V' without its trailing space (648 units).
    0x26044c0477111bbf: "the reference shapes the run without its line's trailing space, which kerns it",
}


def report(requests, outcomes, path):
    """Divergent runs per script, raw and at the engine's cluster granularity, then the normalised ones by script and
    kind with keys for `explain` (the real differences). Runs in REFERENCE_CAUSED are listed apart with their reason."""
    raw_scripts, normalised_scripts = collections.Counter(), collections.Counter()
    by_script = collections.defaultdict(list)
    caused = []
    for key, (_, raw, normalised, script) in outcomes.items():
        if key in REFERENCE_CAUSED:
            if raw or normalised:
                caused.append(key)
            continue
        raw_scripts[str(script)] += bool(raw)
        normalised_scripts[str(script)] += bool(normalised)
        if normalised:
            by_script[(script, normalised)].append(requests[key])
    lines = [f'HarfBuzz {hb.version_string()} runs: {len(outcomes)}, divergent raw: {sum(raw_scripts.values())}, '
             f'at the engine\'s cluster granularity: {sum(normalised_scripts.values())}', '',
             'divergent runs per script: raw -> normalised']
    for script in sorted(raw_scripts, key=lambda s: (-raw_scripts[s], s)):
        if raw_scripts[script]:
            lines.append(f'    {script}: {raw_scripts[script]} -> {normalised_scripts[script]}')
    lines.append('')
    if caused:
        lines.append(f'divergences the reference causes: {len(caused)} runs')
        for key in sorted(caused):
            lines.append(f'    {key:016x} {describe(requests[key])!r}: {REFERENCE_CAUSED[key]}')
        lines.append('')
    for (script, kind), items in sorted(by_script.items(), key=lambda item: (-len(item[1]), str(item[0]))):
        fonts = collections.Counter(font_name(r['font']['path']) for r in items)
        lines.append(f'{script} {kind}: {len(items)} runs ({", ".join(f"{n} {c}" for n, c in fonts.most_common(4))})')
        for request in sorted(items, key=lambda r: (r['run'], r['key']))[:12]:
            lines.append(f'    {request["key"]:016x} {describe(request)!r}')
    text = '\n'.join(lines) + '\n'
    if path:
        Path(path).write_text(text, encoding='utf-8')
    print(text if len(text) < 6000 else text[:6000] + '...\n')


def record(arguments):
    run_tests(arguments.build, arguments.jobs)
    records, requests = recorded_requests()
    # --merge keeps the committed profiles (another machine's fonts and group digests) and adds this machine's; it must
    # come from the same source revision. Without it the reference holds this machine alone.
    reference = read_reference(OUTPUT.with_suffix('.bin')) if arguments.merge else empty_reference()
    add_records(reference, records)
    outcomes = {}
    for key, request in requests.items():
        value, raw, normalised, script = compare(request)
        reference['runs'][key] = value
        outcomes[key] = (value, raw, normalised, script)
        if normalised:
            reference['divergent'].add(key)
        else:
            reference['divergent'].discard(key)
    size = write_reference(reference)
    print(f'{OUTPUT.with_suffix(".bin")}: {len(reference["profiles"])} profiles, {len(reference["fonts"])} fonts, '
          f'{len(reference["runs"])} runs ({len(reference["divergent"])} divergent), {size} bytes')
    coverage(records)
    report(requests, outcomes, arguments.report)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    recording = commands.add_parser('record')
    recording.add_argument('--build', default='D:/MaiCord-build/Release')
    recording.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    recording.add_argument('--merge', action='store_true')
    recording.add_argument('--report')
    reporting = commands.add_parser('report')
    reporting.add_argument('--report')
    explaining = commands.add_parser('explain')
    explaining.add_argument('key')
    arguments = parser.parse_args()
    if arguments.command == 'record':
        record(arguments)
    elif arguments.command == 'report':
        _, requests = recorded_requests()
        report(requests, {key: compare(request) for key, request in requests.items()}, arguments.report)
    else:
        _, requests = recorded_requests()
        request = requests.get(int(arguments.key, 16))
        if not request:
            raise SystemExit(f'run {arguments.key} is not in the last recording ({RECORDS})')
        shaped, script = shape(request)
        print(f'{describe(request)!r} script={script} font={request["font"]["path"]} face={request["font"]["face"]} '
              f'flags={request["flags"]} em={request["em"]} profile={request["profile"]!r}')
        for instance in request['instances']:
            groups, glyphs = native_canonical(instance)
            print('native  ', [(first, count, [glyphs[g] for g in members]) for first, count, members in groups])
        groups, glyphs = harfbuzz_canonical(request, shaped)
        print('harfbuzz', [(first, count, [glyphs[g] for g in members]) for first, count, members in groups])


if __name__ == '__main__':
    main()
