#!/usr/bin/env python3
"""Generate a MaiCord update-signing key pair with Windows CNG.

Writes the private key as a JWK file (store it as the GitHub Actions secret
MAICORD_UPDATE_SIGNING_KEY, then delete the file) and prints the public key
that both the release workflow and the client build (MAICORD_UPDATE_PUBLIC_KEY)
use.

    python tools/release/generate_update_key.py %USERPROFILE%\\maicord-update-key.jwk

The output file must not exist and must be outside the repository.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cng_p256  # noqa: E402

REPOSITORY = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output", help="new private JWK file (outside the repository)")
    arguments = parser.parse_args()
    output = Path(arguments.output).resolve()
    if output == REPOSITORY or REPOSITORY in output.parents:
        print("error: refusing to write a private key inside the repository.", file=sys.stderr)
        return 1
    key = cng_p256.PrivateKey.generate()
    try:
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        print(f"error: {output} already exists; refusing to overwrite a key.", file=sys.stderr)
        return 1
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(key.to_jwk())
    point = key.public_point
    print(f"Private JWK written to {output}")
    print("Store its contents as the MAICORD_UPDATE_SIGNING_KEY secret, keep an offline backup, then delete it.\n")
    print(f"MAICORD_UPDATE_PUBLIC_KEY (repository variable and CMake cache variable):\n{point.hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
