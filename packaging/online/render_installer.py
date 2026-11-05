#!/usr/bin/env python3
"""Writes the one-line installer, install.sh, from install.sh.in with this repository's name and update key.

    python3 packaging/online/render_installer.py --repository maj113/MaiCord --public-key HEX --output dist/install.sh

HEX is the update public key as the repository variable MAICORD_UPDATE_PUBLIC_KEY holds it: the uncompressed P-256 point
(04 || X || Y, 130 hex digits). The script carries it as a PEM public key, which is what `openssl pkeyutl -verify` takes.
The release job attaches the result to the release, so `https://github.com/<repository>/releases/latest/download/install.sh`
is the installer of the latest release. Standard library only.
"""
import argparse
import base64
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# SubjectPublicKeyInfo of an uncompressed id-ecPublicKey / prime256v1 point: the fixed DER header before the 65 bytes.
SPKI_PREFIX = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200')


def pem(public_key: str) -> str:
    if not re.fullmatch(r'04[0-9a-fA-F]{128}', public_key):
        raise SystemExit('error: the public key is not 130 hex digits starting with 04')
    body = base64.b64encode(SPKI_PREFIX + bytes.fromhex(public_key)).decode('ascii')
    lines = [body[i:i + 64] for i in range(0, len(body), 64)]
    return '\n'.join(['-----BEGIN PUBLIC KEY-----', *lines, '-----END PUBLIC KEY-----'])


def render(repository: str, public_key: str) -> str:
    if not re.fullmatch(r'[A-Za-z0-9._-]+/[A-Za-z0-9._-]+', repository):
        raise SystemExit('error: the repository is not owner/name')
    owner, name = repository.split('/')
    text = (HERE / 'install.sh.in').read_text(encoding='utf-8')
    for placeholder, value in (('@REPOSITORY@', repository), ('@OWNER@', owner.lower()), ('@NAME@', name),
                               ('@PUBLIC_KEY_PEM@', pem(public_key))):
        text = text.replace(placeholder, value)
    if re.search(r'@[A-Z_]+@', text):
        raise SystemExit('error: an installer placeholder is left unfilled')
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--repository', required=True, help='owner/name')
    parser.add_argument('--public-key', required=True, help='the update public key, 130 hex digits')
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(render(arguments.repository, arguments.public_key), encoding='utf-8', newline='\n')
    arguments.output.chmod(0o755)
    print(f'{arguments.output}: {arguments.output.stat().st_size} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
