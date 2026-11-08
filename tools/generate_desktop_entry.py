#!/usr/bin/env python3
"""Writes MaiCord's Linux desktop integration files, the build artifact packagers install.

    python tools/generate_desktop_entry.py --output DIR [--exec maicord] [--icons]
    python tools/generate_desktop_entry.py --validate FILE.desktop

DIR gets the layout of an install prefix's share directory:

    applications/io.github.maj113.MaiCord.desktop
    icons/hicolor/<N>x<N>/apps/io.github.maj113.MaiCord.png      with --icons (tools/generate_app_icon.py's PNGs)

The .desktop file is named after the application id, which is also the Wayland app_id the window sets, the well-known
D-Bus name of the single-instance channel (include/maicord/platform/dbus/instance.hpp) and the Unity launcher entry's
desktop id, so launchers, docks and the compositor all find the one entry. MimeType registers MaiCord for the discord://
and maicord:// links; the launcher passes the link as a bare argument (Exec %U), which desktop_launch_parse accepts. Standard
library only.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

APPLICATION_ID = 'io.github.maj113.MaiCord'
REQUIRED_KEYS = ('Type', 'Name', 'Exec', 'Icon', 'Categories', 'MimeType', 'StartupWMClass')
ICON_SIZES = (16, 24, 32, 48, 64, 256)


def entry(exec_name: str) -> str:
    fields = (
        ('Type', 'Application'),
        ('Version', '1.5'),
        ('Name', 'MaiCord'),
        ('GenericName', 'Chat Client'),
        ('Comment', 'A native Discord client'),
        ('Exec', f'{exec_name} %U'),
        ('Icon', APPLICATION_ID),
        ('Terminal', 'false'),
        ('Categories', 'Network;InstantMessaging;Chat;'),
        ('Keywords', 'Discord;Chat;Voice;Messaging;'),
        ('MimeType', 'x-scheme-handler/discord;x-scheme-handler/maicord;'),
        ('StartupNotify', 'true'),
        ('StartupWMClass', APPLICATION_ID),
    )
    return '[Desktop Entry]\n' + ''.join(f'{key}={value}\n' for key, value in fields)


def parse(text: str) -> dict:
    groups, current = {}, None
    for number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith('#'):
            continue
        if line.startswith('['):
            if not line.endswith(']'):
                raise ValueError(f'line {number}: a group header must end with "]"')
            current = groups.setdefault(line[1:-1], {})
            continue
        if current is None or '=' not in line:
            raise ValueError(f'line {number}: not a key=value line inside a group')
        key, value = line.split('=', 1)
        if key in current:
            raise ValueError(f'line {number}: duplicate key {key}')
        current[key] = value
    return groups


def validate(path: Path) -> list:
    problems = []
    try:
        groups = parse(path.read_text(encoding='utf-8'))
    except (OSError, UnicodeDecodeError, ValueError) as error:
        return [str(error)]
    main = groups.get('Desktop Entry')
    if main is None:
        return ['no [Desktop Entry] group']
    for key in REQUIRED_KEYS:
        if not main.get(key):
            problems.append(f'missing {key}')
    if main.get('Type') != 'Application':
        problems.append('Type must be Application')
    if '%U' not in main.get('Exec', '') and '%u' not in main.get('Exec', ''):
        problems.append('Exec must take the link (%u or %U)')
    for key in ('Categories', 'MimeType', 'Keywords'):
        value = main.get(key)
        if value and not value.endswith(';'):
            problems.append(f'{key} must end with ";"')
    mime = main.get('MimeType', '').split(';')
    for scheme in ('x-scheme-handler/discord', 'x-scheme-handler/maicord'):
        if scheme not in mime:
            problems.append(f'MimeType lacks {scheme}')
    if path.name != APPLICATION_ID + '.desktop':
        problems.append(f'the file must be named {APPLICATION_ID}.desktop')
    if main.get('StartupWMClass') != APPLICATION_ID or main.get('Icon') != APPLICATION_ID:
        problems.append('Icon and StartupWMClass must be the application id')
    return problems


def write_icons(output: Path) -> None:
    generator = Path(__file__).with_name('generate_app_icon.py')
    with tempfile.TemporaryDirectory() as scratch:
        pngs = Path(scratch) / 'png'
        subprocess.run([sys.executable, '-B', str(generator), '--output', str(Path(scratch) / 'maicord.ico'),
                        '--png-directory', str(pngs)], check=True, stdout=subprocess.DEVNULL)
        for size in ICON_SIZES:
            source = pngs / f'maicord-{size}-optimized.png'
            if not source.exists():
                continue
            destination = output / 'icons' / 'hicolor' / f'{size}x{size}' / 'apps' / f'{APPLICATION_ID}.png'
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source.read_bytes())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--exec', dest='exec_name', default='maicord', help='the command the entry runs (default: maicord)')
    parser.add_argument('--icons', action='store_true', help='also write the hicolor icon PNGs')
    parser.add_argument('--validate', type=Path, metavar='FILE')
    args = parser.parse_args()
    if args.validate:
        problems = validate(args.validate)
        for problem in problems:
            print(f'{args.validate}: {problem}', file=sys.stderr)
        return 1 if problems else 0
    if not args.output:
        parser.error('--output or --validate is required')
    applications = args.output / 'applications'
    applications.mkdir(parents=True, exist_ok=True)
    target = applications / f'{APPLICATION_ID}.desktop'
    target.write_text(entry(args.exec_name), encoding='utf-8', newline='\n')
    if args.icons:
        write_icons(args.output)
    problems = validate(target)
    for problem in problems:
        print(f'{target}: {problem}', file=sys.stderr)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
