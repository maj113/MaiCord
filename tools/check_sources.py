"""Syntax-checks changed sources against the kept build state, without building.

Needs a build directory produced by `tools/build.ps1 -KeepIntermediates` (add -Test to cover test
sources): compile_commands.json for the commands and the ninja deps log to map a header to every
translation unit that includes it. Each unit is compiled with clang-cl /Zs (parse and analyse, no
output), so nothing in the build directory changes and several checkers can run at once.

  python tools/check_sources.py                  changed files from `git status` (default)
  python tools/check_sources.py src/ui/a.cpp ... named files; headers expand to their includers
  python tools/check_sources.py --all-includers  check every includer of a header (default caps at 60)
"""
import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

REPOSITORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.normpath(os.path.join(REPOSITORY, '..', 'MaiCord-build', 'Release'))
SOURCE = ('.c', '.cpp')
CHECKED = SOURCE + ('.h', '.hpp', '.inc', '.inl')
BELOW_NORMAL = 0x00004000


def key(path):
    return os.path.normcase(os.path.normpath(os.path.abspath(path)))


def changed_files():
    lines = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=all'], cwd=REPOSITORY,
                           capture_output=True, text=True, check=True).stdout.splitlines()
    paths = []
    for line in lines:
        if line[:2].strip() == 'D':
            continue
        path = line[3:].split(' -> ')[-1].strip('"')
        if path.endswith(CHECKED):
            paths.append(os.path.join(REPOSITORY, path))
    return paths


def ninja():
    cache = open(os.path.join(BUILD, 'CMakeCache.txt'), encoding='utf-8').read()
    return re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$', cache, re.M).group(1).strip()


def includers():
    """Header -> source files of every unit whose recorded dependencies list it."""
    text = subprocess.run([ninja(), '-C', BUILD, '-t', 'deps'], capture_output=True, text=True, check=True).stdout
    users = {}
    deps = []

    def flush():
        if deps:
            source = next((d for d in deps if d.endswith(SOURCE)), None)
            if source:
                for d in deps:
                    users.setdefault(key(d), set()).add(key(source))

    for line in text.splitlines():
        if line and not line[0].isspace():
            flush()
            deps = []
        elif line.strip():
            deps.append(line.strip())
    flush()
    return users


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('files', nargs='*')
    parser.add_argument('--all-includers', action='store_true')
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    arguments = parser.parse_args()

    database = os.path.join(BUILD, 'compile_commands.json')
    if not os.path.exists(database):
        sys.exit(f'{database} is missing: run tools/build.ps1 -KeepIntermediates (with -Test for test sources) first.')
    commands = {}
    for entry in json.load(open(database, encoding='utf-8')):
        if entry['file'].endswith(SOURCE) and 'clang-cl' in entry['command']:
            commands.setdefault(key(entry['file']), entry)  # one configuration per source is enough to parse it

    requested = [os.path.join(REPOSITORY, f) for f in arguments.files] or changed_files()
    units, unmapped = set(), []
    users = None
    for path in requested:
        if key(path) in commands:
            units.add(key(path))
            continue
        if users is None:
            users = includers()
        found = sorted(users.get(key(path), ()))
        found = [f for f in found if f in commands]
        if not found:
            unmapped.append(path)
        units.update(found if arguments.all_includers else found[:60])

    def check(unit):
        entry = commands[unit]
        # A precompiled header is stale once a header it holds changes: parse that header instead.
        command = re.sub(r'\s/(F[odp]|Y[cu])\S+', '', entry['command'])
        command = re.sub(r'\s-c(?=\s)', ' /Zs', command).replace(' -flto', '')
        result = subprocess.run(command, cwd=entry['directory'], capture_output=True, text=True,
                                creationflags=BELOW_NORMAL)
        return unit, result.returncode, (result.stdout + result.stderr).strip()

    failures = 0
    with ThreadPoolExecutor(arguments.jobs) as pool:
        for unit, code, output in pool.map(check, sorted(units)):
            if code:
                failures += 1
                print(f'FAIL {os.path.relpath(unit, REPOSITORY)}')
                print('\n'.join(output.splitlines()[:40]))
    for path in unmapped:
        print(f'note: no recorded includer for {os.path.relpath(path, REPOSITORY)} (not built in the kept state)')
    print(f'{len(units) - failures}/{len(units)} units parse cleanly')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
