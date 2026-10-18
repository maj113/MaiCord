"""Fast native-text development checks; large font matrices require --exhaustive."""
from pathlib import Path
import argparse
import atexit
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import threading
import time


# Whole-suite deadlines, independent of individual CTest timeouts. Focused
# suites retain their cases/assertions and avoid running unrelated profiles.
SUITE_TIMEOUTS = {
    'quick': 15,
    'layout': 5,
    'hits': 10,
    'scripts': 5,
    'extended-scripts': 5,
    'devanagari-app': 5,
    'devanagari-direction': 5,
    'direction-tracking': 5,
    'arabic-context': 5,
    'devanagari-tracking': 5,
    'devanagari-memory': 5,
    'font-replay': 5,
    'script-storage': 2,
    'font-instance': 5,
    'font-catalog': 2,
    'font-names': 2,
    'font-family': 2,
    'font-coverage': 2,
    'thai-shaping': 10,
    'thai-wrapping': 5,
    'thai-context': 5,
    'stages': 5,
    'spacing': 5,
    'emoji-run': 5,
    'emoji-app': 5,
    'arabic-run': 5,
    'devanagari-plan': 5,
    'use-plan': 2,
    'myanmar-plan': 2,
    'tibetan-plan': 2,
    'tibetan-input': 2,
    'tibetan-shape': 2,
    'myanmar-input': 2,
    'myanmar-shape': 2,
    'khmer-plan': 2,
    'khmer-input': 2,
    'khmer-shape': 2,
    'sinhala-input': 2,
    'sinhala-shape': 2,
    'tamil-plan': 5,
    'bengali-plan': 5,
    'gujarati-plan': 5,
    'gurmukhi-plan': 5,
    'odia-plan': 5,
    'telugu-plan': 5,
    'kannada-plan': 5,
    'malayalam-plan': 5,
    'gpos-marks': 5,
    'telugu-cpu': 5,
    'kannada-cpu': 5,
    'telugu-app': 5,
    'sinhala-app': 5,
    'myanmar-app': 5,
    'myanmar-tracking': 5,
    'tibetan-app': 5,
    'tibetan-tracking': 5,
    'khmer-app': 5,
    'khmer-tracking': 5,
    'sinhala-tracking': 5,
    'malayalam-app': 5,
    'malayalam-tracking': 5,
    'kannada-app': 5,
    'kannada-tracking': 5,
    'telugu-tracking': 5,
    'odia-app': 5,
    'owned-formats': 5,
    'odia-cpu': 5,
    'odia-tracking': 5,
    'gurmukhi-app': 5,
    'gurmukhi-tracking': 5,
    'gujarati-app': 5,
    'gujarati-tracking': 5,
    'bengali-app': 5,
    'bengali-tracking': 5,
    'tamil-app': 5,
    'tamil-direction': 5,
    'tamil-tracking': 5,
    'tamil-tracking-shape': 5,
    'tamil-direction-shape': 5,
    'long': 10,
    'editor-capacity': 5,
}

SUITE_TARGETS = {
    'stages': 'MaiCordShapeStageTests',
    'spacing': 'MaiCordNativeSpacingTests',
    'emoji-run': 'MaiCordEmojiRunTests',
    'arabic-run': 'MaiCordArabicPlanTests',
    'devanagari-plan': 'MaiCordDevanagariPlanTests',
    'use-plan': 'MaiCordUsePlanTests',
    'myanmar-plan': 'MaiCordUsePlanTests',
    'tibetan-plan': 'MaiCordUsePlanTests',
    'tibetan-input': 'MaiCordTibetanInputTests',
    'tibetan-shape': 'MaiCordTibetanShapeTests',
    'myanmar-input': 'MaiCordMyanmarInputTests',
    'myanmar-shape': 'MaiCordMyanmarShapeTests',
    'khmer-plan': 'MaiCordKhmerPlanTests',
    'khmer-input': 'MaiCordKhmerInputTests',
    'khmer-shape': 'MaiCordKhmerShapeTests',
    'sinhala-input': 'MaiCordSinhalaInputTests',
    'sinhala-shape': 'MaiCordSinhalaShapeTests',
    'tamil-plan': 'MaiCordDevanagariPlanTests',
    'bengali-plan': 'MaiCordDevanagariPlanTests',
    'gujarati-plan': 'MaiCordDevanagariPlanTests',
    'gurmukhi-plan': 'MaiCordDevanagariPlanTests',
    'odia-plan': 'MaiCordDevanagariPlanTests',
    'telugu-plan': 'MaiCordDevanagariPlanTests',
    'kannada-plan': 'MaiCordDevanagariPlanTests',
    'malayalam-plan': 'MaiCordDevanagariPlanTests',
    'gpos-marks': 'MaiCordGposExecuteTests',
    'font-instance': 'MaiCordFontInstanceTests',
    'font-catalog': 'MaiCordFontCatalogTests',
    'font-names': 'MaiCordFontNamesTests',
    'font-family': 'MaiCordFontFamilyTests',
    'font-coverage': 'MaiCordFontCatalogCoverageTests',
}


class CheckFailure(Exception):
    def __init__(self, row):
        self.row = row
        super().__init__(row['name'])


def include_fingerprints(repo, owner):
    """Track the owner's actual transitive .inc inputs, including generated data."""
    pending = [repo / owner]
    seen = set()
    values = {}
    while pending:
        source = pending.pop().resolve()
        if source in seen:
            continue
        seen.add(source)
        data = source.read_bytes()
        if source.suffix == '.inc':
            values[source.relative_to(repo).as_posix()] = hashlib.sha256(data).hexdigest()
        for name in re.findall(rb'^\s*#\s*include\s*"([^"\r\n]+\.inc)"', data, re.MULTILINE):
            dependency = source.parent / name.decode('utf-8')
            if not dependency.is_file():
                raise FileNotFoundError(f'{source}: missing include {dependency}')
            pending.append(dependency)
    return dict(sorted(values.items()))


def run_check(name, command, cwd, logs, timeout, stop, *, env=None):
    before = time.perf_counter()
    log = logs / (name + '.log')
    status = 'passed'
    with log.open('w', encoding='utf-8') as output:
        if stop.is_set():
            status, code = 'cancelled', 130
        else:
            process = subprocess.Popen(command, cwd=cwd, env=env, stdout=output,
                stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            while process.poll() is None:
                if stop.is_set() or time.perf_counter()-before >= timeout:
                    status = 'cancelled' if stop.is_set() else 'timeout'
                    # Kill only this invocation's process tree, including a test
                    # launched by CTest or compiler launched by the build tool.
                    subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                        stdout=output, stderr=subprocess.STDOUT,
                        creationflags=subprocess.CREATE_NO_WINDOW, timeout=5)
                    process.wait(timeout=5)
                    break
                try:
                    process.wait(timeout=0.1)
                except subprocess.TimeoutExpired:
                    pass
            code = 124 if status == 'timeout' else 130 if status == 'cancelled' else process.returncode
            if code and status == 'passed':
                status = 'failed'
    row = dict(name=name, command=command, seconds=time.perf_counter()-before,
        exit=code, status=status, timeout_seconds=timeout, log=str(log))
    print(f"{name}: {row['seconds']:.2f}s, {status}, exit {code}", flush=True)
    if code:
        stop.set()
        if status != 'cancelled':
            print(log.read_text(encoding='utf-8', errors='replace')[-6000:])
        raise CheckFailure(row)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    repo = Path(__file__).resolve().parents[1]
    default = (repo.parent / 'MaiCord-build/Release').resolve()
    parser.add_argument('--build-dir', type=Path, default=default,
        help='Single Release cache; must be <repo-parent>/MaiCord-build/Release.')
    parser.add_argument('--config', choices=('Release',), default='Release')
    parser.add_argument('--output-dir', type=Path, required=True,
        help='Explicit directory for this invocation logs and result. No implicit evidence directory.')
    parser.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument('--suite', choices=tuple(SUITE_TIMEOUTS), nargs='+', action='extend',
        help='Run affected suites together with one incremental build; default: quick. May be repeated.')
    parser.add_argument('--test', action='append', default=[], metavar='NAME',
        help='Run only this exact CTest name within the selected suites. May be repeated.')
    parser.add_argument('--case', type=int, metavar='INDEX',
        help='Diagnose one Indic/direction sample across all its styles, sizes and modes; excludes the separate control corpus.')
    parser.add_argument('--test-timeout', type=float,
        help='Deadline for all selected tests: 5–10s focused, 15s quick; combined budgets capped at 15s.')
    parser.add_argument('--build-timeout', type=float, default=30,
        help='Deadline in seconds for each configure/build step (default: 30; raise explicitly for a cold build).')
    parser.add_argument('--matrix-timeout', type=float, default=20,
        help='Deadline in seconds per explicit exhaustive font check (maximum: 20).')
    parser.add_argument('--reconfigure', action='store_true', help='Force CMake configuration instead of reusing a matching development cache.')
    parser.add_argument('--exhaustive', action='store_true', help='Also run all 21 fonts through the full drawing, hit and plan matrices; intentionally slow.')
    args = parser.parse_args()
    suites = list(dict.fromkeys(args.suite or ['quick']))
    suite_name = '+'.join(suites)
    case_suites = {'myanmar-app', 'myanmar-tracking', 'khmer-app', 'khmer-tracking', 'sinhala-app', 'sinhala-tracking', 'malayalam-app', 'malayalam-tracking', 'kannada-app', 'kannada-tracking', 'telugu-app', 'telugu-tracking', 'odia-app', 'odia-tracking', 'gurmukhi-app', 'gurmukhi-tracking', 'gujarati-app', 'gujarati-tracking', 'bengali-app', 'bengali-tracking', 'tamil-tracking-shape', 'tamil-tracking', 'tamil-direction-shape', 'tamil-direction', 'tamil-app', 'devanagari-app', 'devanagari-tracking', 'devanagari-direction', 'direction-tracking', 'arabic-context'}
    if args.case is not None and (not set(suites) <= case_suites or args.case < 0):
        parser.error('--case requires a nonnegative index and only Indic/direction app suites')
    if args.exhaustive and (args.test or args.case is not None):
        parser.error('--exhaustive cannot be combined with --test or --case')
    if args.exhaustive and suites != ['quick']:
        parser.error('--exhaustive cannot be combined with a focused --suite')
    if args.test_timeout is None:
        args.test_timeout = 2 if args.case is not None else min(15, sum(SUITE_TIMEOUTS[suite] for suite in suites))
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if any(not 0 < value < float('inf') for value in
           (args.test_timeout, args.build_timeout, args.matrix_timeout)):
        parser.error('timeouts must be finite and positive')
    if args.test_timeout > 20 or args.matrix_timeout > 20:
        parser.error('test deadlines must not exceed 20 seconds')
    build = args.build_dir.resolve()
    if build != default:
        parser.error(f'Use the single Release build: {default}')
    build.mkdir(parents=True, exist_ok=True)
    cmake = shutil.which('cmake') or str(Path(os.environ['ProgramFiles']) / 'CMake/bin/cmake.exe')
    ctest = str(Path(cmake).with_name('ctest.exe'))
    logs = args.output_dir.resolve()
    if logs == build or logs.is_relative_to(repo):
        parser.error('Keep output separate from the source tree and Release cache root')
    logs.mkdir(parents=True, exist_ok=True)
    rows = []
    started = time.perf_counter()
    stop = threading.Event()

    def save(status):
        result = dict(mode='exhaustive' if args.exhaustive else suite_name, suites=suites, config=args.config,
            coverage='diagnostic' if args.test or args.case is not None else 'suite',
            tests=args.test, sample=args.case,
            status=status, seconds=time.perf_counter()-started, steps=rows)
        (logs/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        return result

    def run(name, command, timeout=None, *, env=None):
        try:
            row = run_check(name, command, repo, logs,
                args.build_timeout if timeout is None else timeout, stop, env=env)
        except CheckFailure as error:
            rows.append(error.row)
            raise
        rows.append(row)
        return row

    # Save failed runs too; otherwise stale successful evidence can obscure a
    # new hang or failure. The final write below includes all completed steps.
    save('running')
    def save_unfinished():
        save('failed' if stop.is_set() else 'interrupted')
    atexit.register(save_unfinished)

    # Reconfiguring unconditionally regenerates custom rules and can relink the
    # test even without source changes. CMake's build check handles later edits.
    cache_file = build/'CMakeCache.txt'
    cache = {}
    if cache_file.exists():
        for line in cache_file.read_text(encoding='utf-8').splitlines():
            if not line.startswith(('#','//')) and ':' in line and '=' in line:
                key,value=line.split('=',1);cache[key.split(':',1)[0]]=value
    expected = dict(CMAKE_GENERATOR='Ninja',CMAKE_BUILD_TYPE='Release',
        MAICORD_BUILD_TESTS='ON',MAICORD_BUILD_FONT_REFERENCE='OFF',MAICORD_NATIVE_TEXT_LINES='ON',
        MAICORD_NATIVE_GLYPH_OUTLINES='ON',MAICORD_NATIVE_COLR='ON')
    configured = all(cache.get(k)==v for k,v in expected.items()) and Path(cache.get('CMAKE_HOME_DIRECTORY','')).resolve()==repo and Path(cache.get('MAICORD_OUTPUT_DIRECTORY','')).resolve()==build
    if args.reconfigure or not configured:
        run('configure', [cmake,'--preset','x64-release',
            '-DMAICORD_BUILD_TESTS=ON','-DMAICORD_BUILD_FONT_REFERENCE=OFF',
            '-DMAICORD_NATIVE_TEXT_LINES=ON','-DMAICORD_NATIVE_GLYPH_OUTLINES=ON','-DMAICORD_NATIVE_COLR=ON'])
    else:
        print('configure: reused matching cache',flush=True)
    # Some MSBuild tracking caches miss changed .inc files. Invalidate their
    # owners by content, without rebuilding unrelated application targets.
    state_path = build / 'text-include-state.json'
    previous = json.loads(state_path.read_text()) if state_path.exists() else {}
    current = {}
    for owner in ('tests/native_app_line_test.cpp', 'tests/devanagari_plan_test.cpp', 'tests/gpos_execute_test.cpp', 'tests/font_catalog_test.cpp',
                  'tests/arabic_plan_test.cpp', 'tests/sinhala_shape_test.cpp', 'tests/tibetan_shape_test.cpp', 'tests/myanmar_shape_test.cpp', 'tests/khmer_shape_test.cpp', 'tests/khmer_plan_test.cpp', 'src/ui/native_text_candidate.cpp',
                  'src/text/thai_word_break.cpp', 'src/text/tibetan_input.cpp'):
        values = include_fingerprints(repo, owner)
        current[owner] = values
        if previous.get(owner) != values:
            (repo/owner).touch()
    # Record completed invalidations before building. On a timeout or link failure,
    # the next run must reuse successfully compiled objects instead of touching
    # their owners again. MSBuild still tracks unfinished/outdated outputs.
    state_path.write_text(json.dumps(current,indent=2),encoding='utf-8')
    targets = list(dict.fromkeys(SUITE_TARGETS.get(suite, 'MaiCordNativeAppLineTests') for suite in suites))
    run('build-text-target', [cmake,'--build',str(build),'--config',args.config,
        '--target',*targets,'--parallel',str(args.jobs)])
    selection_args = ['-R', '^('+'|'.join(re.escape(name) for name in args.test)+')$'] if args.test else []
    test_env = os.environ.copy()
    # Inherited diagnostic selection must never silently narrow a suite run.
    test_env.pop('MAICORD_DEVANAGARI_SAMPLE', None)
    test_env.pop('MAICORD_HIT_OBSERVE', None)  # Observation streams cannot satisfy acceptance suites.
    if args.case is not None:
        test_env['MAICORD_DEVANAGARI_SAMPLE'] = str(args.case)
        print(f'Diagnostic sample {args.case}: full style/size/mode matrix, separate control corpus excluded.', flush=True)
    run(suite_name+'-tests', [ctest,'--test-dir',str(build),'-C',args.config,
        '-L','^native-text-('+'|'.join(suites)+')$','-j',str(min(4,args.jobs)),'--output-on-failure',
        '--stop-on-failure','--no-tests=error',*selection_args], args.test_timeout, env=test_env)
    if args.exhaustive:
        exe = build / 'MaiCordNativeAppLineTests.exe'
        profiles = [(p.stem,[str(p)]) for p in sorted((repo/'assets/fonts').iterdir()) if p.suffix.lower() in ('.ttf','.otf')]
        profiles += [(name,['--family',name,'400']) for name in ('Segoe UI','Bahnschrift','Georgia','Arial Rounded MT Bold','Consolas','Segoe Print')]
        assert len(profiles)==21
        print('Explicit exhaustive mode: 21 fonts, full drawing/hit/planning matrices.',flush=True)
        for mode in ('trim','hit','plan'):
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                futures = [pool.submit(run,f'{mode}-{name}',[str(exe),f'--multiline-{mode}-profile',*tail],args.matrix_timeout,env=test_env) for name,tail in profiles]
                for future in concurrent.futures.as_completed(futures):
                    future.result()
    atexit.unregister(save_unfinished)
    result = save('passed')
    print(f"{result['mode']} {result['coverage']} verification passed in {result['seconds']:.2f}s. Evidence: {logs}",flush=True)


if __name__ == '__main__':
    try:
        main()
    except CheckFailure as error:
        raise SystemExit(error.row['exit'])
