"""Checks maiopus sources against the official libopus release.

Usage:
  check_upstream.py opus-1.6.1.tar.gz          verify upstream-manifest.json
  check_upstream.py opus-1.6.1.tar.gz --write  refresh statuses and hashes

The tarball is verified against the manifest's SHA-256 and read in memory.
Every file listed in the manifest has one status:
  unchanged  byte-identical to the upstream file
  derived    equal to tools/derive.py applied to the upstream file
  modified   derived, then edited by hand; "changes" lists the divergences
Run from any directory; paths are relative to the maiopus root.
"""
import hashlib
import io
import json
import os
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import derive  # noqa: E402

MANIFEST = os.path.join(ROOT, 'upstream-manifest.json')
SOURCE_DIRS = ('celt', 'silk', 'src', 'include')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read(path):
    with open(os.path.join(ROOT, path), 'rb') as handle:
        return handle.read()


def upstream_members(tarball, expected_sha256):
    with open(tarball, 'rb') as handle:
        data = handle.read()
    if sha256(data) != expected_sha256:
        sys.exit('tarball SHA-256 mismatch')
    members = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for member in archive.getmembers():
            if member.isfile():
                name = member.name.split('/', 1)[1] if '/' in member.name else member.name
                members[name] = archive.extractfile(member).read()
    return members


def tree_sources():
    found = set()
    for top in SOURCE_DIRS:
        for root, _, files in os.walk(os.path.join(ROOT, top)):
            for name in files:
                if name.endswith(('.c', '.h')):
                    found.add(os.path.relpath(os.path.join(root, name), ROOT).replace(os.sep, '/'))
    return found


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    write = '--write' in sys.argv[2:]
    with open(MANIFEST, encoding='utf-8') as handle:
        manifest = json.load(handle)
    upstream = upstream_members(sys.argv[1], manifest['upstream']['sha256'])
    errors = []
    listed = set()
    for entry in manifest['files']:
        path = entry['path']
        listed.add(path)
        if path not in upstream:
            errors.append('%s: not in the upstream release' % path)
            continue
        original = upstream[path]
        current = read(path)
        if write:
            if current == original:
                status = 'unchanged'
            elif current == derive.derive(original):
                status = 'derived'
            else:
                status = 'modified'
            entry['status'] = status
            entry['upstream_sha256'] = sha256(original)
            if status == 'unchanged':
                entry.pop('sha256', None)
            else:
                entry['sha256'] = sha256(current)
            if status == 'modified' and not entry.get('changes'):
                errors.append('%s: modified without a "changes" entry' % path)
            if status != 'modified':
                entry.pop('changes', None)
            continue
        if sha256(original) != entry['upstream_sha256']:
            errors.append('%s: upstream hash differs from the manifest' % path)
        status = entry['status']
        if status == 'unchanged' and current != original:
            errors.append('%s: differs from upstream' % path)
        elif status == 'derived' and current != derive.derive(original):
            errors.append('%s: differs from its derivation' % path)
        elif status == 'modified' and sha256(current) != entry['sha256']:
            errors.append('%s: changed since the manifest was written' % path)
    for path in sorted(tree_sources() - listed):
        errors.append('%s: source file missing from the manifest' % path)
    if write and not errors:
        with open(MANIFEST, 'w', encoding='utf-8', newline='\n') as handle:
            json.dump(manifest, handle, indent=2)
            handle.write('\n')
    for error in errors:
        print(error)
    counts = {}
    for entry in manifest['files']:
        counts[entry['status']] = counts.get(entry['status'], 0) + 1
    print('%d files: %s' % (len(manifest['files']), ', '.join('%d %s' % (n, s) for s, n in sorted(counts.items()))))
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
