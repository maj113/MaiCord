#!/usr/bin/env python3
"""Builds the MaiCord AppImage from the release tarball: MaiCord-<version>-x86_64.AppImage, its .zsync and .sha256.

    python3 packaging/appimage/build_appimage.py --tarball maicord-0.6.6-linux-x86_64.tar.gz --output-dir dist
    python3 packaging/appimage/build_appimage.py --tarball maicord-0.6.6-linux-x86_64-musl.tar.gz --output-dir dist
      the second makes MaiCord-0.6.6-x86_64-musl.AppImage for musl systems (Alpine, Void musl): the same builder around the
      musl tarball's program (the runtime is musl-static, so it starts anywhere; AppRun is the same)
    options: --runtime PATH    a type 2 runtime to use instead of the pinned download (it is not checked against the pin)
             --cache DIR       where the pinned runtime is kept (default $MAICORD_TOOLCHAIN_CACHE, else ~/.cache/maicord-toolchain)

An AppImage is the type 2 runtime (a static executable that mounts the squashfs behind it through FUSE and runs AppRun) with
a SquashFS image of an AppDir appended. The AppDir is the tarball's program with its fonts and licenses, the desktop entry,
icons and AppStream metadata, and packaging/appimage/AppRun. The runtime is the pinned release of packaging/appimage/
runtime.json (SHA-256 checked); it is static (musl), so the AppImage needs no libfuse2 on the host, only a fusermount3 or
fusermount (or APPIMAGE_EXTRACT_AND_RUN=1, which unpacks to a temporary directory and needs no FUSE at all). A release's
runtime carries the zsync update information (`gh-releases-zsync|...`, in its .upd_info section, the same bytes appimagetool
writes), so AppImageUpdate, Gear Lever and the like can update the file; a development build (X.Y.Z-dev.N) carries none.

Needs mksquashfs (squashfs-tools 4.5+) and zsyncmake (zsync). The image is reproducible for one tarball, runtime and
mksquashfs: the SquashFS time stamps are the tarball's SOURCE_DATE_EPOCH-style time (--epoch, else the newest member of
the tarball), entries are root owned, and the .zsync carries the same time. Standard library only.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
APPLICATION_ID = 'io.github.maj113.MaiCord'
PIN = json.loads((HERE / 'runtime.json').read_text(encoding='utf-8'))
UPDATE_SECTION = '.upd_info'


def elf_section(data: bytes, wanted: str) -> tuple[int, int]:
    """(file offset, size) of an ELF64 little-endian section."""
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        raise SystemExit('error: the runtime is not an ELF64 little-endian file')
    section_table, = struct.unpack_from('<Q', data, 0x28)
    entry_size, count, names = struct.unpack_from('<HHH', data, 0x3A)

    def header(index: int) -> tuple[int, int, int]:
        name, _, _, _, offset, size = struct.unpack_from('<IIQQQQ', data, section_table + index * entry_size)
        return name, offset, size
    _, names_offset, _ = header(names)
    for index in range(count):
        name, offset, size = header(index)
        end = data.index(b'\0', names_offset + name)
        if data[names_offset + name:end].decode('ascii') == wanted:
            return offset, size
    raise SystemExit(f'error: the runtime has no {wanted} section')


def elf_end(data: bytes) -> int:
    """The end of the ELF file as the runtime computes it (the section header table's end): the SquashFS starts there."""
    section_table, = struct.unpack_from('<Q', data, 0x28)
    entry_size, count = struct.unpack_from('<HH', data, 0x3A)
    return section_table + entry_size * count


def pinned_runtime(cache: Path) -> bytes:
    runtime = PIN['runtime']
    cached = cache / f"type2-runtime-{runtime['tag']}-{runtime['asset']}"
    if cached.is_file():
        data = cached.read_bytes()
    else:
        print(f"downloading {runtime['url']}")
        with urllib.request.urlopen(runtime['url'], timeout=60) as response:
            data = response.read()
    if hashlib.sha256(data).hexdigest() != runtime['sha256'] or len(data) != runtime['bytes']:
        cached.unlink(missing_ok=True)
        raise SystemExit(f"error: {runtime['asset']} of {runtime['repository']} {runtime['tag']} does not match the pinned SHA-256")
    cache.mkdir(parents=True, exist_ok=True)
    cached.write_bytes(data)
    return data


def extract(tarball: Path, destination: Path) -> Path:
    with tarfile.open(tarball, 'r:gz') as tar:
        if hasattr(tarfile, 'data_filter'):
            tar.extractall(destination, filter='data')
        else:
            tar.extractall(destination)  # noqa: S202 - the tarball is this repository's own output
    (root,) = [path for path in destination.iterdir() if path.is_dir()]
    return root


def newest_member(tarball: Path) -> int:
    with tarfile.open(tarball, 'r:gz') as tar:
        return max(member.mtime for member in tar.getmembers())


def stage(unpacked: Path, appdir: Path, version: str) -> None:
    program = appdir / 'usr' / 'bin'
    program.mkdir(parents=True)
    # The program finds fonts/ and licenses/ next to its own executable.
    shutil.copyfile(unpacked / 'MaiCord', program / 'MaiCord')
    (program / 'MaiCord').chmod(0o755)
    for directory in ('fonts', 'licenses'):
        shutil.copytree(unpacked / directory, program / directory)
    share = appdir / 'usr' / 'share'
    shutil.copytree(unpacked / 'share', share)

    # The AppDir's desktop entry runs the program inside it; --integrate writes the user's own with the AppImage's path.
    entry = share / 'applications' / f'{APPLICATION_ID}.desktop'
    text = re.sub(r'(?m)^Exec=.*$', 'Exec=MaiCord %U', entry.read_text(encoding='utf-8'))
    text = text.rstrip('\n') + f'\nX-AppImage-Name=MaiCord\nX-AppImage-Version={version}\n'
    entry.write_text(text, encoding='utf-8', newline='\n')
    shutil.copyfile(entry, appdir / f'{APPLICATION_ID}.desktop')
    icon = share / 'icons' / 'hicolor' / '256x256' / 'apps' / f'{APPLICATION_ID}.png'
    shutil.copyfile(icon, appdir / f'{APPLICATION_ID}.png')
    (appdir / '.DirIcon').symlink_to(f'{APPLICATION_ID}.png')

    shutil.copyfile(HERE / 'AppRun', appdir / 'AppRun')
    (appdir / 'AppRun').write_bytes((appdir / 'AppRun').read_bytes().replace(b'\r\n', b'\n'))
    (appdir / 'AppRun').chmod(0o755)
    for path in appdir.rglob('*'):
        if path.is_symlink():
            continue
        path.chmod(0o755 if path.is_dir() or path.name in ('MaiCord', 'AppRun') else 0o644)
    appdir.chmod(0o755)


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--tarball', type=Path, required=True, help='maicord-X.Y.Z-linux-x86_64.tar.gz (build_tarball.py)')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, help='a runtime binary instead of the pinned release')
    parser.add_argument('--cache', type=Path, help='where the pinned runtime is cached')
    parser.add_argument('--epoch', type=int, help='time stamp of the image (default: the newest member of the tarball)')
    arguments = parser.parse_args()

    found = re.fullmatch(r'maicord-(\d+\.\d+\.\d+(?:-dev\.\d+)?)-linux-x86_64(-musl)?\.tar\.gz', arguments.tarball.name)
    if not found:
        parser.error(f'{arguments.tarball.name} is not a MaiCord release tarball')
    version = found.group(1)
    libc = found.group(2) or ''
    for tool in ('mksquashfs', 'zsyncmake'):
        if not shutil.which(tool):
            parser.error(f'{tool} is not on PATH (squashfs-tools and zsync)')
    cache = arguments.cache or Path(os.environ.get('MAICORD_TOOLCHAIN_CACHE') or Path.home() / '.cache' / 'maicord-toolchain')
    runtime = arguments.runtime.read_bytes() if arguments.runtime else pinned_runtime(cache)
    epoch = arguments.epoch if arguments.epoch is not None else newest_member(arguments.tarball)

    # A release names where its updates are; a master build's file is not on a release feed.
    info = b'' if '-dev.' in version else PIN['update_information'].replace('@LIBC@', libc).encode('ascii')
    offset, size = elf_section(runtime, UPDATE_SECTION)
    if len(info) >= size:
        raise SystemExit(f'error: the update information is {len(info)} bytes, the runtime has room for {size - 1}')
    runtime = bytearray(runtime)
    runtime[offset:offset + size] = info.ljust(size, b'\0')
    end = elf_end(runtime)
    if end != len(runtime):
        raise SystemExit(f'error: the runtime is {len(runtime)} bytes but its ELF ends at {end}')

    name = f'MaiCord-{version}-x86_64{libc}.AppImage'
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    output = arguments.output_dir / name
    with tempfile.TemporaryDirectory() as scratch:
        scratch = Path(scratch)
        unpacked = extract(arguments.tarball, scratch / 'tarball')
        appdir = scratch / 'AppDir'
        stage(unpacked, appdir, version)
        squashfs = scratch / 'payload.squashfs'
        run(['mksquashfs', str(appdir), str(squashfs), '-noappend', '-quiet', '-no-progress', '-root-owned', '-no-xattrs',
             '-comp', 'zstd', '-Xcompression-level', '19', '-b', '256K',
             '-all-time', str(epoch), '-mkfs-time', str(epoch)])
        output.write_bytes(bytes(runtime) + squashfs.read_bytes())
    output.chmod(0o755)
    os.utime(output, (epoch, epoch))
    zsync = arguments.output_dir / f'{name}.zsync'
    if info:
        zsync.unlink(missing_ok=True)
        # The URL is the file name: zsync and AppImageUpdate resolve it beside the .zsync, on the same release.
        run(['zsyncmake', '-u', name, '-o', str(zsync), str(output)])
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    (arguments.output_dir / f'{name}.sha256').write_text(f'{digest}  {name}\n', encoding='utf-8', newline='\n')
    print(f'{name}: {output.stat().st_size} bytes, sha256 {digest}' + ('' if info else ' (no update information)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
