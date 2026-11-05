#!/usr/bin/env python3
"""Tests the one-line installer (install.sh.in) against a throwaway signing key, with openssl and the stand-in program.

    python3 packaging/online/test_installer.py

It builds the stand-in tarball (packaging/build_tarball.py --stub), signs a manifest the way tools/release/maicord_release.py
does (ECDSA P-256 over SHA-256 of the domain line and the manifest, 64 bytes r || s), lays out a site and a release
directory, and runs the rendered installer over file:// URLs into a private home: the good install, then a wrong key, a
tampered manifest, a tarball of the right size with other bytes, a short tarball, an expired manifest, a manifest of another
architecture and an unknown channel file. Each failure must stop before anything is installed. Needs openssl and sh.
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import render_installer  # noqa: E402

DOMAIN = b'MaiCord update manifest v1\n'
failures = []


def check(condition: bool, message: str) -> None:
    print(('ok   ' if condition else 'FAIL ') + message)
    if not condition:
        failures.append(message)


def openssl(*arguments, data: bytes | None = None) -> bytes:
    return subprocess.run(['openssl', *map(str, arguments)], input=data, capture_output=True, check=True).stdout


def der_signature_to_raw(der: bytes) -> bytes:
    assert der[0] == 0x30
    at = 2 if der[1] < 0x80 else 2 + (der[1] & 0x7F)
    parts = []
    for _ in range(2):
        assert der[at] == 0x02
        length = der[at + 1]
        parts.append(int.from_bytes(der[at + 2:at + 2 + length], 'big').to_bytes(32, 'big'))
        at += 2 + length
    return b''.join(parts)


def manifest(arch: str, issued: int, expires: int, size: int, digest: str, version: str) -> bytes:
    major, minor, patch = (int(part) for part in version.split('.'))
    number = major * 1_000_000_000 + minor * 1_000_000 + patch * 1_000 + 999
    return (f'{{"format":2,"product":"MaiCord","arch":"{arch}","channel":"stable","version":{number},"issued":{issued},'
            f'"expires":{expires},"bytes":{size},"sha256":"{digest}","artifact":"maicord-{version}-linux-x86_64.tar.gz"}}').encode()


def main() -> int:
    version = '0.6.6'
    with tempfile.TemporaryDirectory() as scratch_text:
        scratch = Path(scratch_text)
        release = scratch / 'releases' / f'v{version}'
        subprocess.run([sys.executable, '-B', str(ROOT / 'packaging' / 'build_tarball.py'), '--stub', '--version', version,
                        '--output-dir', str(release)], check=True, capture_output=True)
        name = f'maicord-{version}-linux-x86_64.tar.gz'
        tarball = (release / name).read_bytes()
        digest = hashlib.sha256(tarball).hexdigest()

        key = scratch / 'key.pem'
        openssl('ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', key)
        public = openssl('ec', '-in', key, '-pubout', '-outform', 'DER')[-65:].hex()
        other = scratch / 'other.pem'
        openssl('ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', other)
        other_public = openssl('ec', '-in', other, '-pubout', '-outform', 'DER')[-65:].hex()

        def lay_out(site: Path, document: bytes, signer: Path = key) -> None:
            directory = site / 'updates' / 'p2' / 'linux'
            directory.mkdir(parents=True, exist_ok=True)
            (directory / 'maicord-stable.manifest.json').write_bytes(document)
            sign_digest = hashlib.sha256(DOMAIN + document).digest()
            der = openssl('pkeyutl', '-sign', '-inkey', signer, '-in', '/dev/stdin', data=sign_digest)
            (directory / 'maicord-stable.manifest.json.sig').write_bytes(der_signature_to_raw(der))

        def install(site: Path, key_hex: str, releases: Path, label: str, extra=()) -> subprocess.CompletedProcess:
            script = scratch / f'install-{label}.sh'
            script.write_text(render_installer.render('maj113/MaiCord', key_hex), encoding='utf-8', newline='\n')
            home = scratch / f'home-{label}'
            home.mkdir()
            return subprocess.run(['sh', str(script), '--site', f'file://{site}', '--releases', f'file://{releases}',
                                   '--prefix', str(scratch / f'prefix-{label}'), '--no-url-handler', *extra],
                                  capture_output=True, text=True, env=dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(home / '.config')))

        now = int(time.time())
        good = scratch / 'site-good'
        lay_out(good, manifest('linux-x64', now - 100, now + 86400, len(tarball), digest, version))
        result = install(good, public, scratch / 'releases', 'good')
        installed = scratch / 'prefix-good' / 'opt' / 'MaiCord' / 'MaiCord'
        check(result.returncode == 0 and installed.is_file(), 'the signed release installs: ' + result.stderr.strip())
        check(subprocess.run([str(installed), '--version'], capture_output=True, text=True).stdout.startswith(f'MaiCord {version}'),
              'the installed program runs')

        result = install(good, other_public, scratch / 'releases', 'wrongkey')
        check(result.returncode != 0 and 'signature does not verify' in result.stderr and not (scratch / 'prefix-wrongkey').exists(),
              'a manifest signed by another key is refused')

        tampered = scratch / 'site-tampered'
        lay_out(tampered, manifest('linux-x64', now - 100, now + 86400, len(tarball), digest, version))
        path = tampered / 'updates' / 'p2' / 'linux' / 'maicord-stable.manifest.json'
        path.write_bytes(path.read_bytes().replace(b'"bytes":', b'"bytes": '))
        result = install(tampered, public, scratch / 'releases', 'tampered')
        check(result.returncode != 0 and 'signature does not verify' in result.stderr and not (scratch / 'prefix-tampered').exists(),
              'a changed manifest is refused')

        swapped = scratch / 'releases-swapped' / f'v{version}'
        swapped.mkdir(parents=True)
        (swapped / name).write_bytes(bytes([tarball[0] ^ 1]) + tarball[1:])
        result = install(good, public, scratch / 'releases-swapped', 'swapped')
        check(result.returncode != 0 and 'does not match the signed SHA-256' in result.stderr and not (scratch / 'prefix-swapped').exists(),
              'a tarball of the right size with other bytes is refused')

        short = scratch / 'releases-short' / f'v{version}'
        short.mkdir(parents=True)
        (short / name).write_bytes(tarball[:-1])
        result = install(good, public, scratch / 'releases-short', 'short')
        check(result.returncode != 0 and 'wrong size' in result.stderr and not (scratch / 'prefix-short').exists(),
              'a short tarball is refused')

        expired = scratch / 'site-expired'
        lay_out(expired, manifest('linux-x64', now - 2000, now - 1000, len(tarball), digest, version))
        result = install(expired, public, scratch / 'releases', 'expired')
        check(result.returncode != 0 and 'not current' in result.stderr and not (scratch / 'prefix-expired').exists(),
              'an expired manifest is refused')

        foreign = scratch / 'site-arch'
        lay_out(foreign, manifest('x64', now - 100, now + 86400, len(tarball), digest, version))
        result = install(foreign, public, scratch / 'releases', 'arch')
        check(result.returncode != 0 and 'not linux-x64' in result.stderr and not (scratch / 'prefix-arch').exists(),
              "another system's manifest is refused")

        result = install(good, public, scratch / 'releases', 'channel', ('--channel', 'nightly'))
        check(result.returncode == 2, 'an unknown channel is a usage error')
    print(f'{len(failures)} failure(s)' if failures else 'the installer checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
