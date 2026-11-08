#!/usr/bin/env python3
"""Keeps the hand-written GLSL of the Linux client in step with the HLSL of the Windows client.

Every src/render/shaders/<x>.hlsl the shader pack builds (the maicord_shader(... PACK ...) calls of
cmake/generated_sources.cmake) has a twin src/render/shaders/gles/<x>.glsl, and the first line of the twin records which
HLSL it was written against:

    // twin: src/render/shaders/<x>.hlsl <the first 16 hex digits of the SHA-256 of the HLSL, line ends as LF>

An HLSL file that changes without its twin's line changing fails this check, naming the twin. The remedy is to port the
change to the GLSL (src/render/shaders/gles/README.md says how), review the pair side by side, then record the new hash:

    python tools/check_shader_twins.py --stamp <x>      (one twin at a time: stamping is the statement "this twin matches")

Every build runs the check through tools/check_source_rules.py, again whenever a shader or a twin changes.
"""
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GLSL_DIRECTORY = ROOT / 'src' / 'render' / 'shaders' / 'gles'
HEADER = re.compile(r'^// twin: (\S+) ([0-9a-f]{16})$')


def packed_sources():
    """The HLSL files (repository-relative, POSIX) the shader pack builds."""
    text = (ROOT / 'cmake' / 'generated_sources.cmake').read_text()
    directory = re.search(r'set\(maicord_shaders\s+(\S+)\)', text).group(1)
    sources = []
    for call in re.finditer(r'maicord_shader\(([^)]*)\)', text):
        words = call.group(1).split()
        if 'PACK' in words and words[5].replace('${maicord_shaders}', directory) not in sources:
            sources.append(words[5].replace('${maicord_shaders}', directory))
    return sources


def digest(hlsl):
    data = (ROOT / hlsl).read_bytes().replace(b'\r\n', b'\n')
    return hashlib.sha256(data).hexdigest()[:16]


def twin_of(hlsl):
    return GLSL_DIRECTORY / (pathlib.PurePosixPath(hlsl).stem + '.glsl')


def recorded(twin):
    """(HLSL path, hash) of the first line of a twin, or None."""
    first = twin.read_text().split('\n', 1)[0].rstrip('\r')
    match = HEADER.match(first)
    return match.groups() if match else None


def check():
    problems = []
    sources = packed_sources()
    for hlsl in sources:
        twin = twin_of(hlsl)
        name = twin.relative_to(ROOT).as_posix()
        if not twin.is_file():
            problems.append('%s has no GLSL twin: write %s (src/render/shaders/gles/README.md)' % (hlsl, name))
            continue
        header = recorded(twin)
        if not header:
            problems.append('%s: the first line must be "// twin: %s <hash>"' % (name, hlsl))
        elif header[0] != hlsl:
            problems.append('%s names %s as its twin, but it is the twin of %s' % (name, header[0], hlsl))
        elif header[1] != digest(hlsl):
            problems.append('%s changed since its GLSL twin %s was written against it (recorded %s, now %s): port the '
                            'change, review the pair, then run: python tools/check_shader_twins.py --stamp %s' % (
                                hlsl, name, header[1], digest(hlsl), twin.stem))
    for twin in sorted(GLSL_DIRECTORY.glob('*.glsl')):
        if not any(twin_of(hlsl) == twin for hlsl in sources):
            problems.append('%s is the twin of no packed HLSL file' % twin.relative_to(ROOT).as_posix())
    return problems


def stamp(name):
    for hlsl in packed_sources():
        twin = twin_of(hlsl)
        if twin.stem == name:
            lines = twin.read_text().split('\n')
            lines[0] = '// twin: %s %s' % (hlsl, digest(hlsl))
            twin.write_text('\n'.join(lines), newline='\n')
            print('%s now records %s %s' % (twin.relative_to(ROOT).as_posix(), hlsl, digest(hlsl)))
            return 0
    print('error: no twin named %s (one of: %s)' % (name, ', '.join(twin_of(h).stem for h in packed_sources())))
    return 1


def main(arguments):
    if arguments[:1] == ['--stamp'] and len(arguments) == 2:
        return stamp(arguments[1])
    if arguments:
        print(__doc__)
        return 2
    problems = check()
    for problem in problems:
        print('error: ' + problem)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
