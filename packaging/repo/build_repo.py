#!/usr/bin/env python3
"""Builds MaiCord's signed apt, dnf/zypper and pacman repositories: a directory tree for GitHub Pages.

    python3 packaging/repo/build_repo.py --site SITE/repo --base-url https://maj113.github.io/MaiCord/repo \\
        --deb maicord_0.6.6_amd64.deb --rpm maicord-0.6.6-1.x86_64.rpm --pacman maicord-0.6.6-1-x86_64.pkg.tar.zst

SITE/repo gets (existing files are kept, so each release adds to the repository and the newest --keep versions stay):

    maicord.asc  maicord.gpg      the repository's public key, armored and as a binary keyring (apt's signed-by)
    maicord.sources               deb822 source for apt: `sudo curl -fsSLo /etc/apt/sources.list.d/maicord.sources <url>`
    maicord.repo                  yum/dnf/zypper repository file
    apt/pool/main/m/maicord/*.deb
    apt/dists/stable/{Release,InRelease,Release.gpg}  and  main/binary-amd64/Packages{,.gz,.xz}
    rpm/x86_64/*.rpm  and  rpm/repodata/ with repomd.xml.asc
    arch/x86_64/*.pkg.tar.zst with a detached .sig each, and maicord.{db,files}.tar.gz with .sig, plus copies of them as
                                  maicord.db and maicord.files (the names pacman fetches; Pages has no symlinks)

The metadata is signed (InRelease/Release.gpg for apt; repomd.xml.asc for dnf, checked with repo_gpgcheck=1; the .sig of the
pacman databases) and so is each package (rpmsign, gpgcheck=1; a detached binary signature for each pacman package, also
stored in the database as %PGPSIG%). The pacman database is written here in repo-add's format (a tar.gz of <name>-<version>/desc
and, in the files database, /files), not by repo-add, so that no Arch system is needed; it lists the newest package only, as
repo-add does, and the older ones stay as files for `pacman -U`. The key is the ASCII-armored secret key in
the environment variable MAICORD_REPO_SIGNING_KEY (its passphrase, if it has one, in MAICORD_REPO_SIGNING_PASSPHRASE):
generate it once (`gpg --quick-generate-key "MaiCord Packages <https://github.com/maj113/MaiCord/issues>" ed25519 sign 5y`,
`gpg --armor --export-secret-keys <id>`) and keep the offline backup. It is imported into a private GNUPGHOME that is
removed afterwards. Needs gpg, apt-ftparchive (apt-utils), createrepo_c and tar with zstd; standard library otherwise.
"""
import argparse
import base64
import gzip
import hashlib
import io
import lzma
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

CODENAME = 'stable'
PACKAGE = 'maicord'


def version_key(path: Path) -> tuple:
    found = re.search(r'(\d+)\.(\d+)\.(\d+)', path.name)
    return tuple(int(part) for part in found.groups()) if found else (0, 0, 0)


def prune(directory: Path, pattern: str, keep: int) -> None:
    files = sorted(directory.glob(pattern), key=version_key)
    for stale in files[:-keep] if keep else []:
        stale.unlink()


def run(command, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


class Signer:
    def __init__(self, home: Path, secret_key: str, passphrase: str | None):
        self.home = home
        self.passphrase = passphrase
        home.mkdir(mode=0o700)
        self.environment = dict(os.environ, GNUPGHOME=str(home))
        run(['gpg', '--batch', '--quiet', '--import'], input=secret_key.encode('ascii'), env=self.environment)
        listing = run(['gpg', '--batch', '--with-colons', '--list-secret-keys'], capture_output=True, env=self.environment)
        fingerprints = [line.split(':')[9] for line in listing.stdout.decode().splitlines() if line.startswith('fpr:')]
        if not fingerprints:
            raise SystemExit('error: MAICORD_REPO_SIGNING_KEY holds no secret key')
        self.fingerprint = fingerprints[0]

    def gpg(self, *arguments) -> list:
        command = ['gpg', '--batch', '--yes', '--quiet', '--local-user', self.fingerprint]
        if self.passphrase:
            command += ['--pinentry-mode', 'loopback', '--passphrase-fd', '0']
        return command + [str(argument) for argument in arguments]

    def run(self, *arguments) -> None:
        run(self.gpg(*arguments), input=(self.passphrase or '').encode(), env=self.environment)

    def sign_rpm(self, rpm: Path) -> None:
        # rpmsign runs gpg itself; the passphrase (if any) goes through a file in the private GNUPGHOME.
        passphrase = self.home / 'passphrase'
        passphrase.write_text(self.passphrase or '', encoding='utf-8')
        passphrase.chmod(0o600)
        command = ('%{__gpg} gpg --batch --no-verbose --no-armor --no-secmem-warning --pinentry-mode loopback '
                   f'--passphrase-file {passphrase} -u "%{{_gpg_name}}" -sbo %{{__signature_filename}} --digest-algo sha256 '
                   '%{__plaintext_filename}')
        run(['rpmsign', '--addsign', '--define', f'__gpg {shutil.which("gpg")}', '--define', f'_gpg_name {self.fingerprint}',
             '--define', f'__gpg_sign_cmd {command}', rpm], env=self.environment)

    def public(self, armored: Path, keyring: Path) -> None:
        armored.write_bytes(run(['gpg', '--batch', '--armor', '--export', self.fingerprint], capture_output=True,
                                env=self.environment).stdout)
        keyring.write_bytes(run(['gpg', '--batch', '--export', self.fingerprint], capture_output=True,
                                env=self.environment).stdout)


def build_apt(root: Path, deb: Path, keep: int, signer: Signer) -> None:
    pool = root / 'pool' / 'main' / PACKAGE[0] / PACKAGE
    pool.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(deb, pool / deb.name)
    prune(pool, f'{PACKAGE}_*_amd64.deb', keep)
    binary = root / 'dists' / CODENAME / 'main' / 'binary-amd64'
    binary.mkdir(parents=True, exist_ok=True)
    packages = run(['apt-ftparchive', '--arch', 'amd64', 'packages', 'pool'], capture_output=True, cwd=root).stdout
    (binary / 'Packages').write_bytes(packages)
    (binary / 'Packages.gz').write_bytes(gzip.compress(packages, 9, mtime=0))
    (binary / 'Packages.xz').write_bytes(lzma.compress(packages, preset=9))
    configuration = root / 'release.conf'
    configuration.write_text(
        'APT::FTPArchive::Release {\n'
        '  Origin "MaiCord";\n  Label "MaiCord";\n'
        f'  Suite "{CODENAME}";\n  Codename "{CODENAME}";\n'
        '  Architectures "amd64";\n  Components "main";\n'
        '  Description "MaiCord, an unofficial native Discord client";\n};\n', encoding='utf-8')
    release = run(['apt-ftparchive', '-c', configuration.name, 'release', f'dists/{CODENAME}'], capture_output=True,
                  cwd=root).stdout
    configuration.unlink()
    distribution = root / 'dists' / CODENAME
    (distribution / 'Release').write_bytes(release)
    signer.run('--clearsign', '--output', distribution / 'InRelease', distribution / 'Release')
    signer.run('--armor', '--detach-sign', '--output', distribution / 'Release.gpg', distribution / 'Release')


def build_rpm(root: Path, rpm: Path, keep: int, signer: Signer) -> None:
    packages = root / 'x86_64'
    packages.mkdir(parents=True, exist_ok=True)
    destination = packages / rpm.name
    shutil.copyfile(rpm, destination)
    signer.sign_rpm(destination)
    prune(packages, f'{PACKAGE}-*.x86_64.rpm', keep)
    shutil.rmtree(root / 'repodata', ignore_errors=True)
    run(['createrepo_c', '--quiet', '--general-compress-type', 'xz', root])
    signer.run('--armor', '--detach-sign', '--output', root / 'repodata' / 'repomd.xml.asc', root / 'repodata' / 'repomd.xml')


def pacman_entry(package: Path, signature: Path) -> tuple[str, str, str, int]:
    """The database entry of a package as repo-add writes it: (directory, desc, files, build date)."""
    with tarfile.open(fileobj=io.BytesIO(run(['zstd', '-dc', package], capture_output=True).stdout)) as archive:
        pkginfo = archive.extractfile('.PKGINFO')
        if pkginfo is None:
            raise SystemExit(f'error: {package.name} has no .PKGINFO')
        text = pkginfo.read().decode('utf-8')
        # The files database lists every member that is not a dot file (.PKGINFO, .MTREE, .INSTALL), directories with a slash.
        names = {member.name + '/' if member.isdir() else member.name
                 for member in archive.getmembers() if not member.name.startswith('.')}
    # .PKGINFO is "key = value" lines; repo-add keeps the last value of a plain key and every value of the list keys, and
    # normalizes runs of whitespace.
    info: dict[str, list[str]] = {}
    for line in text.splitlines():
        key, separator, value = line.partition('=')
        if separator and not line.startswith('#'):
            info.setdefault(key.strip(), []).append(' '.join(value.split()))

    def last(key: str) -> list[str]:
        return info[key][-1:] if key in info else []

    def every(key: str) -> list[str]:
        return info.get(key, [])

    name, version = last('pkgname'), last('pkgver')
    if not name or not version:
        raise SystemExit(f'error: {package.name} is not a pacman package')
    fields = [
        ('FILENAME', [package.name]), ('NAME', name), ('BASE', last('pkgbase')), ('VERSION', version), ('DESC', last('pkgdesc')),
        ('GROUPS', every('group')), ('CSIZE', [str(package.stat().st_size)]), ('ISIZE', last('size')),
        ('SHA256SUM', [hashlib.sha256(package.read_bytes()).hexdigest()]),
        ('PGPSIG', [base64.b64encode(signature.read_bytes()).decode('ascii')]),
        ('URL', last('url')), ('LICENSE', every('license')), ('ARCH', last('arch')), ('BUILDDATE', last('builddate')),
        ('PACKAGER', last('packager')), ('REPLACES', every('replaces')), ('CONFLICTS', every('conflict')),
        ('PROVIDES', every('provides')), ('DEPENDS', every('depend')), ('OPTDEPENDS', every('optdepend')),
        ('MAKEDEPENDS', every('makedepend')), ('CHECKDEPENDS', every('checkdepend')),
    ]
    desc = ''.join(f'%{field}%\n' + ''.join(f'{value}\n' for value in values) + '\n'
                   for field, values in fields if values and values[0])
    files = '%FILES%\n' + ''.join(f'{member}\n' for member in sorted(names, key=str.encode))
    built = last('builddate')
    return f'{name[0]}-{version[0]}', desc, files, int(built[0]) if built else 0


def write_pacman_database(path: Path, directory: str, members: dict[str, str], mtime: int) -> None:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.PAX_FORMAT) as archive:
        folder = tarfile.TarInfo(directory)
        folder.type, folder.mode, folder.mtime, folder.uname, folder.gname = tarfile.DIRTYPE, 0o755, mtime, 'root', 'root'
        archive.addfile(folder)
        for name, text in members.items():
            data = text.encode('utf-8')
            member = tarfile.TarInfo(f'{directory}/{name}')
            member.size, member.mode, member.mtime, member.uname, member.gname = len(data), 0o644, mtime, 'root', 'root'
            archive.addfile(member, io.BytesIO(data))
    path.write_bytes(gzip.compress(buffer.getvalue(), 9, mtime=0))


def build_pacman(root: Path, package: Path, keep: int, signer: Signer) -> None:
    packages = root / 'x86_64'
    packages.mkdir(parents=True, exist_ok=True)
    destination = packages / package.name
    shutil.copyfile(package, destination)
    signer.run('--detach-sign', '--output', f'{destination}.sig', destination)
    prune(packages, f'{PACKAGE}-*-x86_64.pkg.tar.zst', keep)
    for signature in packages.glob('*.pkg.tar.zst.sig'):
        if not signature.with_suffix('').exists():
            signature.unlink()
    kept = sorted(packages.glob(f'{PACKAGE}-*-x86_64.pkg.tar.zst'), key=version_key)
    for old in kept:
        if not Path(f'{old}.sig').exists():
            signer.run('--detach-sign', '--output', f'{old}.sig', old)
    # Like repo-add the database holds the newest version only; the older packages stay beside it for `pacman -U`.
    directory, desc, files, built = pacman_entry(kept[-1], Path(f'{kept[-1]}.sig'))
    # The database is named for the repository, the [maicord] section of pacman.conf; pacman fetches maicord.db and
    # maicord.db.sig (and the .files pair for `pacman -F`). repo-add links those to the .tar.gz files, GitHub Pages has no
    # symlinks, so they are copies.
    for kind, members in (('db', {'desc': desc}), ('files', {'desc': desc, 'files': files})):
        archive = packages / f'{PACKAGE}.{kind}.tar.gz'
        write_pacman_database(archive, directory, members, built)
        signer.run('--detach-sign', '--output', f'{archive}.sig', archive)
        shutil.copyfile(archive, packages / f'{PACKAGE}.{kind}')
        shutil.copyfile(f'{archive}.sig', packages / f'{PACKAGE}.{kind}.sig')


def write_sources(site: Path, base_url: str, armored: Path) -> None:
    # deb822 puts the key in the file itself (apt 2.4+: Debian 12, Ubuntu 22.04 and newer): continuation lines start with
    # a space and an empty line of the armor is a lone dot.
    key = '\n'.join(' ' + (line if line else '.') for line in armored.read_text(encoding='ascii').strip().splitlines())
    (site / 'maicord.sources').write_text(
        'Types: deb\n'
        f'URIs: {base_url}/apt\n'
        f'Suites: {CODENAME}\n'
        'Components: main\n'
        'Architectures: amd64\n'
        f'Signed-By:\n{key}\n', encoding='utf-8', newline='\n')
    (site / 'maicord.repo').write_text(
        '[maicord]\n'
        'name=MaiCord\n'
        f'baseurl={base_url}/rpm\n'
        'enabled=1\n'
        'gpgcheck=1\n'
        'repo_gpgcheck=1\n'
        f'gpgkey={base_url}/maicord.asc\n', encoding='utf-8', newline='\n')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--site', type=Path, required=True, help='the repository directory (SITE/repo of the Pages site)')
    parser.add_argument('--base-url', required=True, help='where --site is served, without a trailing slash')
    parser.add_argument('--deb', type=Path, required=True)
    parser.add_argument('--rpm', type=Path, required=True)
    parser.add_argument('--pacman', type=Path, required=True, help='maicord-X.Y.Z-1-x86_64.pkg.tar.zst')
    parser.add_argument('--keep', type=int, default=3, help='versions of each package to keep (default 3)')
    arguments = parser.parse_args()
    secret_key = os.environ.get('MAICORD_REPO_SIGNING_KEY', '')
    if not secret_key.strip():
        parser.error('set MAICORD_REPO_SIGNING_KEY to the armored secret key of the repository')
    for tool in ('gpg', 'apt-ftparchive', 'createrepo_c', 'zstd'):
        if not shutil.which(tool):
            parser.error(f'{tool} is not on PATH (gnupg, apt-utils, createrepo-c, zstd)')
    if not re.fullmatch(r'https://[^\s/]+(/[^\s/]+)*', arguments.base_url):
        parser.error('--base-url is not an https URL without a trailing slash')
    arguments.site.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        signer = Signer(Path(scratch) / 'gnupg', secret_key, os.environ.get('MAICORD_REPO_SIGNING_PASSPHRASE'))
        build_apt(arguments.site / 'apt', arguments.deb, arguments.keep, signer)
        build_rpm(arguments.site / 'rpm', arguments.rpm, arguments.keep, signer)
        build_pacman(arguments.site / 'arch', arguments.pacman, arguments.keep, signer)
        signer.public(arguments.site / 'maicord.asc', arguments.site / 'maicord.gpg')
    write_sources(arguments.site, arguments.base_url, arguments.site / 'maicord.asc')
    print(f'{arguments.site}: apt, rpm and pacman repositories signed with {signer.fingerprint}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
