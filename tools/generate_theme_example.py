#!/usr/bin/env python3
"""Generate assets/themes/example.maicord-theme.json from the token tables.

Reads include/maicord/ui/theme_tokens.inc (semantic tokens, scales, motion),
include/maicord/ui/theme.hpp (core palette defaults) and theme_styles.hpp
(typography and metric defaults), and writes a version 2 document listing
every token with its dark default. Semantic defaults keep their references
so the example also documents the token tiers. Stdlib only.

  generate_theme_example.py           rewrite the example
  generate_theme_example.py --check   exit 1 when the example is stale
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
UI = ROOT / 'include' / 'maicord' / 'ui'
OUTPUT = ROOT / 'assets' / 'themes' / 'example.maicord-theme.json'
NUMBER = r'-?(?:\d+\.?\d*|\.\d+)f?'


def number(text):
    value = float(text.strip().rstrip('fF'))
    return int(value) if value == int(value) else round(value, 6)


def color(components):
    """Hex when every channel is an exact 8-bit value, else the exact array."""
    values = [float(c.strip().rstrip('fF')) for c in components]
    channels = [round(v * 255) for v in values]
    if all(abs(c / 255 - v) < 1e-7 for c, v in zip(channels, values)):
        text = '#' + ''.join('%02x' % c for c in channels)
        return text[:7] if channels[3] == 255 else text
    return [number(c) for c in components]


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


def core_palette():
    source = strip_comments((UI / 'theme.hpp').read_text(encoding='utf-8'))
    body = re.search(r'struct Theme \{(.*?)\n\};', source, re.S).group(1)
    names = re.findall(r'^\s*Color (\w+);', body, re.M)
    initializer = re.search(r'kDefaultTheme = ui_theme_resolve_defaults\(Theme\{(.*?)\}\);', source, re.S).group(1)
    values = re.findall(r'\{\s*(%s)\s*,\s*(%s)\s*,\s*(%s)\s*,\s*(%s)\s*\}' % ((NUMBER,) * 4), initializer)
    assert len(names) == len(values) == 20, (len(names), len(values))
    return {name: color(value) for name, value in zip(names, values)}


def default_value(expression):
    expression = expression.strip()
    m = re.fullmatch(r'P\((\w+)\)', expression)
    if m:
        return '{colors.%s}' % m.group(1)
    m = re.fullmatch(r'PA\((\w+),\s*(%s)\)' % NUMBER, expression)
    if m:
        return '{colors.%s}@%s' % (m.group(1), number(m.group(2)))
    m = re.fullmatch(r'T\((\w+),\s*(\w+)\)', expression)
    if m:
        return '{semantic.%s.%s}' % (m.group(1), m.group(2).rstrip('_'))
    m = re.fullmatch(r'RGBA\((%s),\s*(%s),\s*(%s),\s*(%s)\)' % ((NUMBER,) * 4), expression)
    if m:
        return color([c.rstrip('fF') for c in m.groups()])
    raise ValueError('unknown token default: ' + expression)


def split_arguments(text):
    depth, parts, current = 0, [], ''
    for c in text:
        if c == ',' and depth == 0:
            parts.append(current)
            current = ''
            continue
        depth += c == '('
        depth -= c == ')'
        current += c
    return parts + [current]


def token_tables():
    source = strip_comments((UI / 'theme_tokens.inc').read_text(encoding='utf-8'))
    semantic, scale, motion = {}, {}, {}
    for args in re.findall(r'^MAICORD_THEME_TOKEN\((.*)\)\s*$', source, re.M):
        group, name, dark, _light = [part.strip() for part in split_arguments(args)]
        semantic.setdefault(group, {})[name.rstrip('_')] = default_value(dark)
    for name, value, _low, _high in re.findall(
            r'^MAICORD_THEME_SCALE\((\w+),\s*(%s),\s*(%s),\s*(%s)\)' % ((NUMBER,) * 3), source, re.M):
        scale[name] = number(value)
    for name, duration, curve in re.findall(r'^MAICORD_THEME_MOTION\((\w+),\s*(\d+),\s*(\w+)\)', source, re.M):
        motion[name] = {'duration_ms': int(duration), 'curve': curve}
    return semantic, scale, motion


def struct_defaults(source, struct):
    body = re.search(r'struct %s \{(.*?)\n\};' % struct, source, re.S).group(1)
    return {name: number(value) for name, value in re.findall(r'(\w+)\{(%s)\}' % NUMBER, body)}


def document():
    styles = strip_comments((UI / 'theme_styles.hpp').read_text(encoding='utf-8'))
    typography = struct_defaults(styles, 'UiThemeTypography')
    typography.update({'body_face': 'normal', 'code_face': 'mono'})
    semantic, scale, motion = token_tables()
    return {
        'version': '2.0',
        'name': 'Example (MaiCord defaults)',
        'base': 'dark',
        'palette': {'blurple': core_palette()['accent']},
        'colors': dict(core_palette(), accent='{palette.blurple}'),
        'semantic': semantic,
        'scale': scale,
        'motion': motion,
        'typography': typography,
        'metrics': struct_defaults(styles, 'UiThemeMetrics'),
    }


def main(argv):
    text = json.dumps(document(), indent=2) + '\n'
    # Keep each colour array on one line.
    text = re.sub(r'\[\s+([^\[\]{}]*?)\s+\]',
                  lambda m: '[' + ', '.join(part.strip() for part in m.group(1).split(',')) + ']', text)
    if '--check' in argv:
        current = OUTPUT.read_text(encoding='utf-8') if OUTPUT.exists() else ''
        if current != text:
            print('%s is stale; run tools/generate_theme_example.py' % OUTPUT.relative_to(ROOT).as_posix())
            return 1
        print('theme example is current')
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(text, encoding='utf-8', newline='\n')
    print('wrote %s' % OUTPUT.relative_to(ROOT).as_posix())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
