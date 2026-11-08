#!/usr/bin/env python3
"""Checks that the hand-written package recipes agree with nfpm/nfpm.yaml and with each other.

    python3 packaging/check_recipes.py

nfpm.yaml is the source of the dependency lists, because it makes the packages the release ships. The recipes that
repeat them for other builders must hold the same entries (in any order):

    debian/debian/control           Depends (but ${misc:Depends}), Recommends, Suggests  = nfpm's deb overrides
    rpm/maicord.spec                Requires, Recommends, Suggests outside the openSUSE branch  = nfpm's rpm overrides
    aur/maicord-bin/PKGBUILD,
    aur/maicord/PKGBUILD            depends, optdepends  = nfpm's archlinux depends and suggests

The recipes that take a release tarball must all name the same release: the AUR, Alpine and Void versions, the spec's
Version and the openSUSE _service download, the Gentoo ebuild's file name and the default of nix/package-bin.nix.
Standard library only; exits 1 and names each difference.
"""
import re
import shlex
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIELDS = ('depends', 'recommends', 'suggests')
VERSIONS = (
    ('aur/maicord-bin/PKGBUILD', r'^pkgver=(\S+)$'),
    ('aur/maicord/PKGBUILD', r'^pkgver=(\S+)$'),
    ('alpine/APKBUILD', r'^pkgver=(\S+)$'),
    ('void/maicord-bin/template', r'^version=(\S+)$'),
    ('rpm/maicord.spec', r'^Version:\s+(\S+)$'),
    ('rpm/_service', r'/releases/download/v([^/]+)/'),
    ('nix/package-bin.nix', r'^\s*version \? "([^"]+)",$'),
)
problems = []


def unquote(item: str) -> str:
    return item[1:-1].replace("''", "'") if item.startswith("'") else item


def nfpm_overrides() -> dict[str, dict[str, list[str]]]:
    """overrides.<packager>.<field> of nfpm.yaml: the file's own shape (two-space indents, one list item per line)."""
    overrides: dict[str, dict[str, list[str]]] = {}
    inside = False
    packager = field = ''
    for line in (HERE / 'nfpm' / 'nfpm.yaml').read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        indent = len(line) - len(line.lstrip())
        if indent == 0:
            inside = line == 'overrides:'
        elif inside and indent == 2:
            packager = line.strip().rstrip(':')
            overrides[packager] = {}
        elif inside and indent == 4:
            field = line.strip().rstrip(':')
            overrides[packager][field] = []
        elif inside and line.lstrip().startswith('- '):
            overrides[packager][field].append(unquote(line.strip()[2:]))
    return overrides


def control_fields() -> dict[str, list[str]]:
    text = (HERE / 'debian' / 'debian' / 'control').read_text(encoding='utf-8')
    fields = {}
    for name, value in re.findall(r'^(Depends|Recommends|Suggests):(.*(?:\n .*)*)', text, re.MULTILINE):
        entries = [entry.strip() for entry in value.split(',')]
        fields[name.lower()] = [entry for entry in entries if entry and entry != '${misc:Depends}']
    return fields


def spec_fields() -> dict[str, list[str]]:
    fields: dict[str, list[str]] = {field: [] for field in FIELDS}
    suse = False
    for line in (HERE / 'rpm' / 'maicord.spec').read_text(encoding='utf-8').splitlines():
        if line.startswith('%if 0%{?suse_version}'):
            suse = True
        elif line.startswith(('%else', '%endif')):
            suse = False
        found = re.match(r'(Requires|Recommends|Suggests):\s+(.+)$', line)
        if found and not suse:
            fields['depends' if found.group(1) == 'Requires' else found.group(1).lower()].append(found.group(2).strip())
    return fields


def pkgbuild_array(path: Path, name: str) -> list[str]:
    found = re.search(rf'^{name}=\((.*?)\)$', path.read_text(encoding='utf-8'), re.MULTILINE | re.DOTALL)
    if not found:
        problems.append(f'{path.relative_to(HERE)} has no {name}=(...)')
        return []
    return shlex.split(found.group(1))


def compare(recipe: str, field: str, found: list[str], expected: list[str]) -> None:
    missing = sorted(set(expected) - set(found))
    extra = sorted(set(found) - set(expected))
    if missing or extra:
        problems.append(f'{recipe} {field} differs from nfpm.yaml: missing {missing}, extra {extra}')


def main() -> int:
    nfpm = nfpm_overrides()
    control = control_fields()
    spec = spec_fields()
    for field in FIELDS:
        compare('debian/debian/control', field, control.get(field, []), nfpm['deb'][field])
        compare('rpm/maicord.spec', field, spec[field], nfpm['rpm'][field])
    for recipe in ('aur/maicord-bin/PKGBUILD', 'aur/maicord/PKGBUILD'):
        compare(recipe, 'depends', pkgbuild_array(HERE / recipe, 'depends'), nfpm['archlinux']['depends'])
        compare(recipe, 'optdepends', pkgbuild_array(HERE / recipe, 'optdepends'), nfpm['archlinux']['suggests'])

    versions = {}
    for recipe, pattern in VERSIONS:
        found = re.findall(pattern, (HERE / recipe).read_text(encoding='utf-8'), re.MULTILINE)
        if len(found) != 1:
            problems.append(f'{recipe} names {len(found)} release versions, not one')
        versions[recipe] = found[0] if found else None
    for ebuild in (HERE / 'gentoo').glob('*.ebuild'):
        versions[ebuild.relative_to(HERE).as_posix()] = ebuild.name.removeprefix('maicord-bin-').removesuffix('.ebuild')
    if len(set(versions.values())) != 1:
        problems.append('the recipes name different releases: ' + ', '.join(f'{recipe} {version}' for recipe, version in versions.items()))

    for problem in problems:
        print(f'check_recipes: {problem}', file=sys.stderr)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
