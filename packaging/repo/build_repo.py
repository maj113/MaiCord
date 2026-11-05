#!/usr/bin/env python3
"""Builds MaiCord's signed apt and dnf/zypper repositories: a directory tree for GitHub Pages.

    python3 packaging/repo/build_repo.py --site SITE/repo --base-url https://maj113.github.io/MaiCord/repo \\
        --deb maicord_0.6.6_amd64.deb --rpm maicord-0.6.6-1.x86_64.rpm

SITE/repo gets (existing files are kept, so each release adds to the repository and the newest --keep versions stay):

    maicord.asc  maicord.gpg      the repository's public key, armored and as a binary keyring (apt's signed-by)
    maicord.sources               deb822 source for apt: `sudo curl -fsSLo /etc/apt/sources.list.d/maicord.sources <url>`
    maicord.repo                  yum/dnf/zypper repository file
    apt/pool/main/m/maicord/*.deb
    apt/dists/stable/{Release,InRelease,Release.gpg}  and  main/binary-amd64/Packages{,.gz,.xz}
    rpm/x86_64/*.rpm  and  rpm/repodata/ with repomd.xml.asc

The metadata is signed (InRelease/Release.gpg for apt; repomd.xml.asc for dnf, checked with repo_gpgcheck=1) and so is each
rpm (rpmsign, gpgcheck=1). The key is the ASCII-armored secret key in
the environment variable MAICORD_REPO_SIGNING_KEY (its passphrase, if it has one, in MAICORD_REPO_SIGNING_PASSPHRASE):
generate it once (`gpg --quick-generate-key "MaiCord Packages <https://github.com/maj113/MaiCord/issues>" ed25519 sign 5y`,
`gpg --armor --export-secret-keys <id>`) and keep the offline backup. It is imported into a private GNUPGHOME that is
removed afterwards. Needs gpg, apt-ftparchive (apt-utils) and createrepo_c; standard library otherwise.
"""
import argparse
import gzip
import lzma
import os
import re
import shutil
import subprocess
import sys
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
    parser.add_argument('--keep', type=int, default=3, help='versions of each package to keep (default 3)')
    arguments = parser.parse_args()
    secret_key = os.environ.get('MAICORD_REPO_SIGNING_KEY', '')
    if not secret_key.strip():
        parser.error('set MAICORD_REPO_SIGNING_KEY to the armored secret key of the repository')
    for tool in ('gpg', 'apt-ftparchive', 'createrepo_c'):
        if not shutil.which(tool):
            parser.error(f'{tool} is not on PATH (gnupg, apt-utils, createrepo-c)')
    if not re.fullmatch(r'https://[^\s/]+(/[^\s/]+)*', arguments.base_url):
        parser.error('--base-url is not an https URL without a trailing slash')
    arguments.site.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        signer = Signer(Path(scratch) / 'gnupg', secret_key, os.environ.get('MAICORD_REPO_SIGNING_PASSPHRASE'))
        build_apt(arguments.site / 'apt', arguments.deb, arguments.keep, signer)
        build_rpm(arguments.site / 'rpm', arguments.rpm, arguments.keep, signer)
        signer.public(arguments.site / 'maicord.asc', arguments.site / 'maicord.gpg')
    write_sources(arguments.site, arguments.base_url, arguments.site / 'maicord.asc')
    print(f'{arguments.site}: apt and rpm repositories signed with {signer.fingerprint}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
