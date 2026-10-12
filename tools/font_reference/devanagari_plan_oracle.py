"""Bounded Devanagari plan/run comparisons (offline QA, no runtime dependency).

fontTools reads metadata and writes temporary per-face fonts only. Windows
supplies expected glyphs/placements; the native executable uses first-party code.
The diagnostic clusters and optional ordered corpus do not establish general
Devanagari shaping or app admission.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import threading
import time


def identity(path):
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def batch_glyphs(reference, font, cases, *, seed='41', detailed=False, independent=False, rtl=False, timeout=3):
    """Reuse one Windows face/analyzer for a bounded stream of Latin-script pairs."""
    payload = bytearray(b'MCSHPB1\0' + struct.pack('<I', len(cases)))
    lengths = []
    for case in cases:
        raw = ''.join(map(chr, case)).encode('utf-16-le')
        lengths.append(len(raw) // 2)
        payload.extend(struct.pack('<II', lengths[-1], int(independent) | (int(rtl) << 1)))
        payload.extend(raw)
    run = subprocess.run([str(reference), str(font), seed, 'default', '1148846080', '0', '0', 'en-US'],
                         input=payload, capture_output=True, timeout=timeout,
                         env=dict(os.environ, MAICORD_SHAPE_BATCH='1'), creationflags=subprocess.CREATE_NO_WINDOW)
    if run.returncode or run.stdout[:12] != payload[:12]:
        raise RuntimeError('Invalid form batch response: ' + repr(run.stderr))
    cursor, result = 12, []
    for length in lengths:
        count = struct.unpack_from('<I', run.stdout, cursor)[0]
        cursor += 4
        glyphs = list(struct.unpack_from('<' + 'H' * count, run.stdout, cursor))
        cursor += count*2
        clusters = list(struct.unpack_from('<' + 'H' * length, run.stdout, cursor))
        cursor += length*2
        placements = [list(struct.unpack_from('<III', run.stdout, cursor+i*12)) for i in range(count)]
        cursor += count*12
        result.append(dict(glyphs=glyphs, clusters=clusters, placements=placements) if detailed else glyphs)
    if cursor != len(run.stdout):
        raise RuntimeError('Truncated or trailing form batch data')
    return result


def form_checks(font, face, native_rows, reference, output, *, script_tag='dev2', block=0x900, timeout=3):
    """Expose ONLY locl + the tested form through Latin ccmp on private scalars.

    This tests context-free GSUB classification without invoking Windows Indic
    syllable eligibility/reordering. Original glyphs, GDEF, lookup graph and
    flags remain intact; no font is installed or original mapping overwritten.
    """
    from fontTools.ttLib.sfnt import SFNTWriter

    table = font['GSUB'].table
    script = next(r.Script for r in table.ScriptList.ScriptRecord if r.ScriptTag == script_tag)
    language = next((r.LangSys for r in script.LangSysRecord if r.LangSysTag == 'ENG '), script.DefaultLangSys)
    selected = {}
    for index in language.FeatureIndex:
        record = table.FeatureList.FeatureRecord[index]
        selected.setdefault(record.FeatureTag, record.Feature.LookupListIndex)
    if language.ReqFeatureIndex != 65535:
        record = table.FeatureList.FeatureRecord[language.ReqFeatureIndex]
        selected[record.FeatureTag] = record.Feature.LookupListIndex
    original_maps = [(t, t.cmap.copy()) for t in font['cmap'].tables if t.isUnicode()]
    best = font.getBestCmap().copy()
    rows = [r for r in native_rows if r.get('face') == face and 'form' in r]
    if len(rows) != 5:
        raise RuntimeError('Expected five native form probes for each face')
    checks = []
    try:
        for cmap, _ in original_maps:
            cmap.cmap[0xe100] = best[block+0x4d]
            for scalar, *_ in rows[0]['cases']:
                cmap.cmap[0xe000 + scalar - block] = best[scalar]
        # Preserve the complete raw lookup graph. Its unsigned internal offsets
        # remain relative to their original owners; only the small directories
        # in front change. Avoid recompiling hundreds of unrelated lookups and
        # glyph outlines for each feature experiment.
        raw_tables = {tag: font.reader[tag] for tag in font.reader.keys()}
        original_gsub = raw_tables['GSUB']
        if original_gsub[:4] != b'\0\1\0\0':
            raise RuntimeError('Raw form fixture currently requires static GSUB 1.0')
        lookup_graph = original_gsub[struct.unpack_from('>H', original_gsub, 8)[0]:]
        raw_tables['cmap'] = font['cmap'].compile(font)
        scripts = struct.pack('>H4s7H', 1, b'latn', 8, 4, 0, 0, 65535, 1, 0)
        for row in rows:
            mask = row['form']
            tag = {2: 'rphf', 8: 'half', 16: 'pref', 4: 'blwf', 32: 'pstf'}[mask]
            roots = sorted(set(selected.get('locl', ())) | set(selected[tag])) if tag in selected else []
            features = struct.pack('>H4s3H', 1, b'ccmp', 8, 0, len(roots)) + struct.pack('>' + 'H' * len(roots), *roots)
            raw_tables['GSUB'] = struct.pack('>I3H', 0x10000, 10, 10 + len(scripts),
                10 + len(scripts) + len(features)) + scripts + features + lookup_graph
            path = output / f'form-{face}-{tag}.ttf'
            with path.open('wb') as destination:
                writer = SFNTWriter(destination, len(raw_tables), font.sfntVersion)
                for table_tag, data in sorted(raw_tables.items()):
                    writer[table_tag] = data
                writer.close()
            cases = [[0xe000 + scalar - block, 0xe100] if mask in (2, 8) else
                     [0xe100, 0xe000 + scalar - block] for scalar, *_ in row['cases']]
            expected = batch_glyphs(reference, path, cases, timeout=timeout)
            for actual, glyphs in zip(row['cases'], expected):
                scalar, formed, glyph, operations = actual
                expected_form = len(glyphs) == 1
                checks.append(dict(kind='form', face=face, scalar=scalar, feature=tag,
                    exact=bool(formed) == expected_form and (not expected_form or glyph == glyphs[0]),
                    native=actual, reference_glyphs=glyphs))
    finally:
        for cmap, mapping in original_maps:
            cmap.cmap = mapping
    return checks


def verify(args, output):
    from fontTools.ttLib import TTCollection, TTFont

    before = time.perf_counter()
    native = subprocess.run([str(args.native), str(args.font)], capture_output=True,
                            text=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW,
                            env=dict(os.environ, MAICORD_DEVANAGARI_RTL=str(int(args.rtl))))
    (output / 'native.log').write_text(native.stdout + native.stderr, encoding='utf-8')
    if native.returncode:
        raise RuntimeError('Native probe failed; see ' + str(output / 'native.log'))
    rows = [json.loads(line) for line in native.stdout.splitlines() if line.startswith('{')]
    # Open the same face data in the single-face Windows observer. No font is
    # installed, and the original file is never rewritten.
    is_collection = args.font.read_bytes()[:4] == b'ttcf'
    fonts = TTCollection(args.font).fonts if is_collection else [TTFont(args.font)]
    substitutions = [('locl',), ('nukt',), ('akhn',), ('rphf',), ('rkrf',), ('pref',),
                     ('blwf',), ('half',), ('pstf',), ('vatu',), ('cjct',), (),
                     ('pres', 'abvs', 'blws', 'psts', 'haln', 'calt', 'rclt')]
    masks = [0, 1, 0, 2, 0, 16, 4, 8, 32, 64, 128, 0, 0]
    checks = []
    for face_index, font in enumerate(fonts):
        font.recalcTimestamp = False
        path = output / ('face-' + str(face_index) + '.ttf')
        font.save(path)
        for kind, name in enumerate(('GSUB', 'GPOS')):
            table = font[name].table
            script = next(record.Script for record in table.ScriptList.ScriptRecord if record.ScriptTag == 'dev2')
            language = next((record.LangSys for record in script.LangSysRecord if record.LangSysTag == 'ENG '), script.DefaultLangSys)
            selected = {}
            for index in language.FeatureIndex:
                record = table.FeatureList.FeatureRecord[index]
                selected.setdefault(record.FeatureTag, record.Feature.LookupListIndex)
            expected = []
            groups = substitutions if not kind else [('kern', 'dist', 'abvm', 'blwm', 'mark', 'mkmk')]
            for stage, tags in enumerate(groups):
                if not kind and stage == 11:
                    continue
                indices = set()
                for tag in tags:
                    indices.update(selected.get(tag, ()))
                if language.ReqFeatureIndex != 65535:
                    indices.update(table.FeatureList.FeatureRecord[language.ReqFeatureIndex].Feature.LookupListIndex)
                expected += [[index, 0 if kind else masks[stage], stage] for index in sorted(indices)]
            actual = next(row['steps'] for row in rows if row.get('kind') == kind and row['face'] == face_index)
            checks.append(dict(kind='plan', face=face_index, table=name, exact=actual == expected,
                               expected=expected, actual=actual))
        for sample, text in enumerate(('915', '915,93c')):
            actual = next(row for row in rows if row.get('sample') == sample and row['face'] == face_index)
            for typography in ('default', 'liga=0,clig=0,kern=0'):
                reference = subprocess.run([str(args.reference), str(path), text, typography,
                                            '1148846080', '0', '0', 'en-US'], capture_output=True,
                                           text=True, timeout=3, creationflags=subprocess.CREATE_NO_WINDOW)
                if reference.returncode:
                    raise RuntimeError(reference.stdout + reference.stderr)
                expected = json.loads(reference.stdout)
                valid = expected['hresult'] == expected['placement_hresult'] == 0
                exact = valid and expected['glyphs'] == actual['glyphs'] and expected['placement_bits'] == actual['placements']
                checks.append(dict(kind='shape', face=face_index, sample=sample, typography=typography,
                                   exact=exact, reference=expected, native=actual))
        if args.forms:
            checks.extend(form_checks(font, face_index, rows, args.reference, output))
        if args.ordered:
            native_rows = [row for row in rows if row.get('face') == face_index and 'ordered' in row]
            from devanagari_shape_samples import samples
            if [row['scalars'] for row in native_rows] != [list(map(ord, text)) for text in samples()]:
                raise RuntimeError('Native ordered corpus differs from its source generator')
            for independent in (False, True):
                expected_rows = batch_glyphs(args.reference, path, [row['scalars'] for row in native_rows],
                                            seed='915', detailed=True, independent=independent, rtl=args.rtl)
                for actual, expected in zip(native_rows, expected_rows):
                    exact = actual['status'] == 0 and all(actual[key] == expected[key] for key in ('glyphs', 'clusters', 'placements'))
                    checks.append(dict(kind='ordered-shape', face=face_index, typography='independent' if independent else 'default', rtl=args.rtl,
                                       exact=exact, native=actual, reference=expected))
    result = dict(status='passed' if all(row['exact'] for row in checks) else 'failed',
                  seconds=time.perf_counter() - before, cases=len(checks),
                  scope='Diagnostic clusters, optional font-form probes and selected ordered inputs; no general script/app admission',
                  inputs=[identity(path) for path in (args.font, args.native, args.reference)], checks=checks)
    (output / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f"{sum(row['exact'] for row in checks)}/{len(checks)} exact plan/run checks in {result['seconds']:.2f}s. Evidence: {output}")
    return 0 if result['status'] == 'passed' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font', required=True, type=Path)
    parser.add_argument('--native', required=True, type=Path)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--timeout', type=float, default=10,
                        help='Whole comparison deadline in seconds (default: 10).')
    parser.add_argument('--forms', action='store_true', help='Also compare context-free consonant form probes using batched Windows lookups.')
    parser.add_argument('--ordered', action='store_true', help='Compare experimental ordered clusters/words in both app typography modes.')
    parser.add_argument('--rtl', action='store_true', help='Use RTL positioning for ordered clusters; plans/form probes are direction-independent.')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.rtl and not args.ordered:
        parser.error('--rtl requires --ordered')
    if not 0 < args.timeout < float('inf'):
        parser.error('--timeout must be finite and positive')
    if args.worker:
        return verify(args, args.output_dir)
    output = args.output_dir / ('run-' + str(time.time_ns()))
    output.mkdir(parents=True)
    result_path = output / 'result.json'
    result_path.write_text(json.dumps(dict(status='running')), encoding='utf-8')
    # Reuse the development runner's deadline/process-tree cleanup. Bounding
    # each child independently would multiply the worst-case wait by face count.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from verify_native_text import CheckFailure, run_check
    command = [sys.executable, str(Path(__file__).resolve()), '--worker',
               '--font', str(args.font.resolve()), '--native', str(args.native.resolve()),
               '--reference', str(args.reference.resolve()), '--output-dir', str(output.resolve())]
    if args.forms:
        command.append('--forms')
    if args.ordered:
        command.append('--ordered')
    if args.rtl:
        command.append('--rtl')
    try:
        row = run_check('devanagari-comparison', command, Path.cwd(), output,
                        args.timeout, threading.Event())
    except CheckFailure as error:
        row = error.row
        result = json.loads(result_path.read_text(encoding='utf-8'))
        result.update(status=row['status'], execution=row)
        result_path.write_text(json.dumps(result, indent=2), encoding='utf-8')
        return row['exit']
    result = json.loads(result_path.read_text(encoding='utf-8'))
    result['execution'] = row
    result_path.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f"{result['cases']} exact plan/run checks. Evidence: {output}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
