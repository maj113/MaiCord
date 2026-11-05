#!/usr/bin/env python3
"""Builds the Linux release tarball: maicord-<version>-linux-x86_64.tar.gz and its .sha256.

    python3 packaging/build_tarball.py --binary PATH --version 0.6.6 --output-dir DIR
    python3 packaging/build_tarball.py --stub --output-dir DIR        a stand-in program, until the client links

The archive holds one directory, maicord-<version>-linux-x86_64/:

    MaiCord                    the program (PATH, or the stub)
    fonts/  licenses/          the bundled fonts and license notices, as next to MaiCord.exe (cmake/client.cmake)
    share/applications/        io.github.maj113.MaiCord.desktop      (tools/generate_desktop_entry.py)
    share/icons/hicolor/       the application icon PNGs             (tools/generate_app_icon.py)
    share/metainfo/            io.github.maj113.MaiCord.metainfo.xml (packaging/metainfo, with this version's release)
    .maicord-portable          the marker of the self-updating channel (packaging/README.md)
    install.sh uninstall.sh    per-user installation under ~/.local
    README.md LICENSE

The distribution packages (AUR, Nix -bin, Flatpak, nfpm) install this tarball's contents; only the tarball install
keeps the marker, which is what lets the program replace itself. The archive is reproducible: sorted entries, root
ownership, a fixed modification time (SOURCE_DATE_EPOCH, else the date of --date, else the commit date) and no gzip
timestamp. Standard library only.
"""
import argparse
import datetime
import gzip
import hashlib
import io
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPLICATION_ID = 'io.github.maj113.MaiCord'
MARKER = '.maicord-portable'
MARKER_TEXT = 'maicord-portable 1\n'
EXECUTABLES = ('MaiCord', 'install.sh', 'uninstall.sh')
LICENSE_SOURCES = (
    ('third_party/opus/COPYING', 'Opus-BSD.txt'),
    ('LICENSE', 'MaiCord-MPL-2.0.txt'),
    ('assets/licenses/Twemoji-graphics.txt', None),
    ('assets/licenses/Unicode-3.0.txt', None),
    ('assets/licenses/ICU-Thai-dictionary.txt', None),
    ('assets/licenses/PyThaiNLP-corpus.txt', None),
    ('assets/licenses/gemoji-MIT.txt', None),
    ('assets/licenses/emojibase-MIT.txt', None),
    ('assets/licenses/MaiCord-sounds.txt', None),
    ('assets/licenses/Lucide-ISC.txt', None),
)
STUB = """#!/bin/sh
# A stand-in for the MaiCord program (packaging/build_tarball.py --stub): the packaging is built and tested before the
# Linux client links. It answers --version and does nothing else.
case "${1:-}" in
    --version) echo "MaiCord @VERSION@ (stub)" ;;
    *) echo "This is the packaging stub of MaiCord @VERSION@, not the client." >&2; exit 1 ;;
esac
"""


def project_version() -> str:
    text = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    match = re.search(r'project\(MaiCord VERSION (\d+\.\d+\.\d+)', text)
    if not match:
        raise SystemExit('error: no project(MaiCord VERSION x.y.z) in CMakeLists.txt')
    return match.group(1)


def epoch(arguments) -> int:
    if os.environ.get('SOURCE_DATE_EPOCH'):
        return int(os.environ['SOURCE_DATE_EPOCH'])
    if arguments.date:
        return int(datetime.datetime.strptime(arguments.date, '%Y-%m-%d').replace(tzinfo=datetime.timezone.utc).timestamp())
    try:
        out = subprocess.run(['git', 'log', '-1', '--format=%ct'], cwd=ROOT, capture_output=True, text=True, check=True)
        return int(out.stdout.strip())
    except (OSError, subprocess.CalledProcessError, ValueError):
        return 315532800  # 1980-01-01, the Windows archives' fixed time


def write_metainfo(destination: Path, version: str, date: str) -> None:
    template = (ROOT / 'packaging' / 'metainfo' / f'{APPLICATION_ID}.metainfo.xml.in').read_text(encoding='utf-8')
    # A master build has no release page of its own.
    page = 'releases' if '-dev.' in version else f'releases/tag/v{version}'
    release = (f'<releases>\n    <release version="{version}" date="{date}">\n'
               f'      <url type="details">https://github.com/maj113/MaiCord/{page}</url>\n'
               f'    </release>\n  </releases>')
    if '@RELEASES@' not in template:
        raise SystemExit('error: the metainfo template has no @RELEASES@')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(template.replace('@RELEASES@', release), encoding='utf-8', newline='\n')


def stage(arguments, directory: Path, version: str, date: str) -> None:
    directory.mkdir(parents=True)
    program = directory / 'MaiCord'
    if arguments.stub:
        program.write_text(STUB.replace('@VERSION@', version), encoding='utf-8', newline='\n')
    else:
        shutil.copyfile(arguments.binary, program)
    program.chmod(0o755)

    fonts = directory / 'fonts'
    fonts.mkdir()
    for source in sorted((ROOT / 'assets' / 'fonts').iterdir()):
        if source.suffix in ('.ttf', '.otf') or (source.name.startswith('OFL-') and source.suffix == '.txt'):
            shutil.copyfile(source, fonts / source.name)
    licenses = directory / 'licenses'
    licenses.mkdir()
    for source, name in LICENSE_SOURCES:
        shutil.copyfile(ROOT / source, licenses / (name or Path(source).name))

    subprocess.run([sys.executable, '-B', str(ROOT / 'tools' / 'generate_desktop_entry.py'), '--output',
                    str(directory / 'share'), '--exec', 'maicord', '--icons'], check=True)
    write_metainfo(directory / 'share' / 'metainfo' / f'{APPLICATION_ID}.metainfo.xml', version, date)

    (directory / MARKER).write_text(MARKER_TEXT, encoding='utf-8', newline='\n')
    for script in ('install.sh', 'uninstall.sh'):
        target = directory / script
        shutil.copyfile(ROOT / 'packaging' / 'tarball' / script, target)
        target.write_bytes(target.read_bytes().replace(b'\r\n', b'\n'))
        target.chmod(0o755)
    for name in ('README.md', 'LICENSE'):
        shutil.copyfile(ROOT / name, directory / name)


def archive(directory: Path, output: Path, mtime: int) -> None:
    names = sorted(path for path in directory.rglob('*'))
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.GNU_FORMAT) as tar:
        top = tarfile.TarInfo(directory.name)
        top.type, top.mode, top.mtime, top.uname, top.gname = tarfile.DIRTYPE, 0o755, mtime, 'root', 'root'
        tar.addfile(top)
        for path in names:
            info = tar.gettarinfo(str(path), arcname=f'{directory.name}/{path.relative_to(directory).as_posix()}')
            info.uid = info.gid = 0
            info.uname = info.gname = 'root'
            info.mtime = mtime
            if info.isdir():
                info.mode = 0o755
            else:
                relative = path.relative_to(directory).as_posix()
                # The mode bit is the host's: a Windows host has none, so the executables are named.
                info.mode = 0o755 if relative in EXECUTABLES or path.stat().st_mode & stat.S_IXUSR else 0o644
            if info.isfile():
                with path.open('rb') as stream:
                    tar.addfile(info, stream)
            else:
                tar.addfile(info)
    with output.open('wb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, compresslevel=9, mtime=0) as packed:
        packed.write(buffer.getvalue())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--binary', type=Path, help='the MaiCord program')
    parser.add_argument('--stub', action='store_true', help='use a stand-in program (the client does not link yet)')
    parser.add_argument('--version', help='X.Y.Z, or X.Y.Z-dev.N for a master build (default: the project version of CMakeLists.txt)')
    parser.add_argument('--date', help='YYYY-MM-DD of the release (default: the commit date)')
    parser.add_argument('--output-dir', type=Path, required=True)
    arguments = parser.parse_args()
    if bool(arguments.binary) == arguments.stub:
        parser.error('give exactly one of --binary and --stub')
    if arguments.binary and not arguments.binary.is_file():
        parser.error(f'{arguments.binary} is not a file')
    version = arguments.version or project_version()
    if not re.fullmatch(r'\d+\.\d+\.\d+(-dev\.\d+)?', version):
        parser.error(f'version {version} is not X.Y.Z or X.Y.Z-dev.N')
    mtime = epoch(arguments)
    date = datetime.datetime.fromtimestamp(mtime, datetime.timezone.utc).strftime('%Y-%m-%d')
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    name = f'maicord-{version}-linux-x86_64'
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch) / name
        stage(arguments, directory, version, arguments.date or date)
        output = arguments.output_dir / f'{name}.tar.gz'
        archive(directory, output, mtime)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    (arguments.output_dir / f'{output.name}.sha256').write_text(f'{digest}  {output.name}\n', encoding='utf-8', newline='\n')
    print(f'{output.name}: {output.stat().st_size} bytes, sha256 {digest}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
