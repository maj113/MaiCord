#!/usr/bin/env python3
"""Ratchet presentation literals in src/ui against tools/theme_literal_baseline.json.

Counts hardcoded colours and corner radii that should come from theme tokens
(Theme::tokens / Theme::scale). The check fails when any count rises above the
committed baseline; lower counts pass and can be committed with --update.
Stdlib only.

  check_theme_literals.py            check (exit 1 on regression)
  check_theme_literals.py --list     print every counted site
  check_theme_literals.py --update   rewrite the baseline to the current counts
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ROOT / 'src' / 'ui'
BASELINE = ROOT / 'tools' / 'theme_literal_baseline.json'

NUMBER = r'-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?f?'
# {r, g, b, a} with every component a literal in [0, 1]; {0, 0, 0, 0} is "none".
QUAD = re.compile(r'\{\s*(%s)\s*,\s*(%s)\s*,\s*(%s)\s*,\s*(%s)\s*\}' % ((NUMBER,) * 4))
# Literal white/black with a computed alpha, e.g. {1, 1, 1, hover ? .16f : .10f}.
NEUTRAL = re.compile(r'\{\s*([01](?:\.0*)?f?)\s*,\s*\1\s*,\s*\1\s*,(?![^}]*\b(?:true|false)\b)'
                     r'(?!\s*(%s)\s*\})[^{};]+\}' % NUMBER)
# Calls whose corner radius argument (0-based index) is a presentation value.
RADIUS_CALLS = {
    'paint_stream_push_rounded': 6,
    'paint_stream_push_rounded_border': 7,
    'ui_background_paint_surface': 5,
}
RADIUS_CALL = re.compile(r'\b(%s)\s*\(' % '|'.join(RADIUS_CALLS))
LITERAL_ARG = re.compile(r'^\s*%s\s*$' % NUMBER)


# A comment, or a string or character literal (an unterminated one runs to the end of the text).
BLANKED = re.compile(r'//[^\n]*|/\*.*?(?:\*/|\Z)|"(?:[^"\\]|\\.)*(?:"|\\?\Z)|\'(?:[^\'\\]|\\.)*(?:\'|\\?\Z)', re.S)


def blank(match):
    text = match.group(0)
    if text[0] == '/':
        return re.sub(r'[^\n]', ' ', text)
    return text[0] + ' ' * (len(text) - 2) + text[0] if len(text) >= 2 else text


def strip(text):
    """Blank comments and string/char literals, keeping offsets and newlines."""
    return BLANKED.sub(blank, text)


def arguments(text, start):
    """Top-level arguments of the call whose '(' is at start."""
    depth, args, current = 0, [], start + 1
    for i in range(start, len(text)):
        c = text[i]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                args.append(text[current:i])
                return args
        elif c == ',' and depth == 1:
            args.append(text[current:i])
            current = i + 1
    return args


def top_level_commas(text):
    depth = commas = 0
    for c in text:
        depth += c in '([{'
        depth -= c in ')]}'
        commas += c == ',' and depth == 0
    return commas


def scan():
    sites = {'color_quad': [], 'color_neutral': [], 'radius_literal': []}
    for path in sorted(SOURCES.rglob('*')):
        if path.suffix not in ('.cpp', '.hpp', '.inc'):
            continue
        text = strip(path.read_text(encoding='utf-8', errors='replace'))
        rel = path.relative_to(ROOT).as_posix()

        def line(offset):
            return text.count('\n', 0, offset) + 1
        for m in QUAD.finditer(text):
            values = [float(v.rstrip('fF')) for v in m.groups()]
            if all(0 <= v <= 1 for v in values) and any(values):
                sites['color_quad'].append((rel, line(m.start()), m.group(0)))
        for m in NEUTRAL.finditer(text):
            alpha = m.group(0).split(',', 3)[3][:-1]
            if top_level_commas(alpha) or re.match(r'^\s*\w+::\w+\s*$', alpha):
                continue  # an aggregate of more fields or an enum, not a colour
            sites['color_neutral'].append((rel, line(m.start()), ' '.join(m.group(0).split())))
        for m in RADIUS_CALL.finditer(text):
            args = arguments(text, m.end() - 1)
            index = RADIUS_CALLS[m.group(1)]
            if index < len(args) and LITERAL_ARG.match(args[index]) and float(args[index].strip().rstrip('fF')):
                sites['radius_literal'].append((rel, line(m.start()), m.group(1) + ' radius ' + args[index].strip()))
    return sites


def main(argv):
    sites = scan()
    counts = {key: len(value) for key, value in sites.items()}
    if '--list' in argv:
        for key, entries in sites.items():
            for rel, number, snippet in entries:
                print('%s:%d: %s %s' % (rel, number, key, snippet))
    if '--update' in argv:
        BASELINE.write_text(json.dumps(counts, indent=2, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
        print('baseline updated: %s' % counts)
        return 0
    baseline = json.loads(BASELINE.read_text(encoding='utf-8'))
    failed = False
    for key, count in sorted(counts.items()):
        allowed = baseline.get(key, 0)
        status = 'ok' if count <= allowed else 'REGRESSION'
        failed |= count > allowed
        print('%-15s %4d / baseline %4d  %s' % (key, count, allowed, status))
    if failed:
        print('Presentation literals increased in src/ui. Use Theme::tokens / Theme::scale '
              '(include/maicord/ui/theme/tokens.inc); see --list.')
        return 1
    if counts != baseline:
        print('Counts fell below the baseline; ratchet it with --update.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
