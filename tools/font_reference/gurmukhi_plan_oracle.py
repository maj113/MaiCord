"""Five-second Gurmukhi planning check; Windows form comparison uses face zero only."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

from devanagari_plan_oracle import form_checks, identity

STAGES = [('locl',), ('nukt',), ('akhn',), ('rphf',), ('rkrf',), ('pref',),
          ('blwf',), ('half',), ('pstf',), ('vatu',), ('cjct',), (),
          ('pres', 'abvs', 'blws', 'psts', 'haln', 'calt', 'rclt')]
MASKS = [0, 1, 0, 2, 0, 16, 4, 8, 32, 64, 128, 0, 0]


def expected_plan(font, table_name):
    table = font[table_name].table
    script = next(r.Script for r in table.ScriptList.ScriptRecord if r.ScriptTag == 'gur2')
    language = next((r.LangSys for r in script.LangSysRecord if r.LangSysTag == 'ENG '), script.DefaultLangSys)
    features = [table.FeatureList.FeatureRecord[i] for i in language.FeatureIndex]
    required = None if language.ReqFeatureIndex == 65535 else table.FeatureList.FeatureRecord[language.ReqFeatureIndex]
    result = []
    for stage, tags in enumerate(STAGES if table_name == 'GSUB' else [('kern', 'dist', 'abvm', 'blwm', 'mark', 'mkmk')]):
        if table_name == 'GSUB' and stage == 11:
            continue
        roots = {}
        for feature in features:
            if feature is required or feature.FeatureTag not in tags:
                continue
            mask = MASKS[stage] if table_name == 'GSUB' else 0
            for lookup in feature.Feature.LookupListIndex:
                roots[lookup] = roots.get(lookup, 0) | (mask or 256)
        if required:
            for lookup in required.Feature.LookupListIndex:
                roots[lookup] = roots.get(lookup, 0) | ((MASKS[stage] if table_name == 'GSUB' else 0) or 256)
        result.extend([lookup, 0 if mask & 256 else mask, stage] for lookup, mask in sorted(roots.items()))
    return result


def verify(args):
    from fontTools.ttLib import TTCollection
    started = time.perf_counter()
    env = dict(os.environ)
    for key in ('MAICORD_DEVANAGARI_BATCH', 'MAICORD_TAMIL_BATCH', 'MAICORD_TAMIL_RUN', 'MAICORD_DEVANAGARI_MEMO_BENCH'):
        env.pop(key, None)
    native = subprocess.run([str(args.native), '--gurmukhi-profile'], capture_output=True, text=True,
                            timeout=2, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
    (args.output_dir / 'native.log').write_text(native.stdout + native.stderr, encoding='utf-8')
    if native.returncode:
        raise RuntimeError('Native Gurmukhi profile failed; see native.log')
    rows = [json.loads(line) for line in native.stdout.splitlines() if line.startswith('{')]
    font_path = Path(os.environ['WINDIR']) / 'Fonts/Nirmala.ttc'
    inputs = {key: identity(path) for key, path in {'font': font_path, 'reference': args.reference,
        'form_oracle': Path(__file__).with_name('devanagari_plan_oracle.py'), 'oracle': Path(__file__)}.items()}
    checks = []
    fonts = TTCollection(font_path, lazy=True)
    try:
        for face, font in enumerate(fonts.fonts):
            for kind, table in enumerate(('GSUB', 'GPOS')):
                actual = next(r['steps'] for r in rows if r.get('face') == face and r.get('kind') == kind)
                expected = expected_plan(font, table)
                checks.append(dict(kind='plan', face=face, table=table, exact=actual == expected, native=actual, expected=expected))
        if args.observations:
            saved = json.loads(args.observations.read_text(encoding='utf-8'))
            if saved['inputs'] != inputs or saved['status'] != 'passed':
                raise RuntimeError('Reference observation identity/status mismatch')
            observations = {(r['face'], r['scalar'], r['feature']): r['reference_glyphs'] for r in saved['checks'] if r['kind'] == 'form'}
            for row in rows:
                if row.get('face') != 0 or 'form' not in row:
                    continue
                tag = {2:'rphf',8:'half',16:'pref',4:'blwf',32:'pstf'}[row['form']]
                for actual in row['cases']:
                    scalar, formed, glyph, _ = actual
                    expected = observations.pop((0, scalar, tag))
                    exact = bool(formed) == (len(expected) == 1) and (not formed or glyph == expected[0])
                    checks.append(dict(kind='form', face=0, scalar=scalar, feature=tag, exact=exact, native=actual, reference_glyphs=expected))
            if observations:
                raise RuntimeError('Unused reference form observations')
        else:
            checks.extend(form_checks(fonts.fonts[0], 0, rows, args.reference, args.output_dir, script_tag='gur2', block=0xa00, timeout=2))
    finally:
        fonts.close()
    if len(checks) <= 12:
        raise RuntimeError('Missing form observations')
    result = dict(status='passed' if all(r['exact'] for r in checks) else 'failed', seconds=time.perf_counter()-started,
        scope='Six Nirmala face plans; face-zero context-free consonant forms only. No Gurmukhi shaping/app admission.',
        inputs=inputs, native=identity(args.native), cases=len(checks), checks=checks)
    (args.output_dir / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return 0 if result['status'] == 'passed' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--observations', type=Path)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        return verify(args)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    result_path = args.output_dir / 'result.json'
    if args.observations and args.observations.resolve() == result_path.resolve():
        parser.error('Keep saved observations separate from the current result')
    result_path.write_text('{"status":"running"}', encoding='utf-8')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from verify_native_text import CheckFailure, run_check
    command = [sys.executable, str(Path(__file__).resolve()), '--worker', '--native', str(args.native.resolve()),
        '--reference', str(args.reference.resolve()), '--output-dir', str(args.output_dir.resolve())]
    if args.observations:
        command.extend(['--observations', str(args.observations.resolve())])
    try:
        execution = run_check('gurmukhi-plan-comparison', command, Path.cwd(), args.output_dir, 5, threading.Event())
    except CheckFailure as error:
        result = json.loads(result_path.read_text(encoding='utf-8'))
        result.update(status=error.row['status'], execution=error.row)
        result_path.write_text(json.dumps(result, indent=2), encoding='utf-8')
        return error.row['exit']
    result = json.loads(result_path.read_text(encoding='utf-8'))
    result['execution'] = execution
    result_path.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f"{result['cases']} exact Gurmukhi plan/form checks in {result['seconds']:.2f}s. Evidence: {args.output_dir}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
