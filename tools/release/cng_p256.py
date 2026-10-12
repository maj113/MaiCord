"""ECDSA P-256 through Windows CNG (bcrypt.dll), using only the Python stdlib.

MaiCord verifies update manifests with BCryptVerifySignature over a SHA-256
digest and a 64-byte IEEE P1363 (r || s) signature. This module performs the
matching publisher operations with the same provider, so no third-party
cryptography package is needed on a Windows release runner.

Keys are exchanged as JSON Web Keys (RFC 7517/7518):
    {"kty": "EC", "crv": "P-256", "x": "<b64url>", "y": "<b64url>", "d": "<b64url>"}
"""

from __future__ import annotations

import base64
import ctypes
import json
import sys
from ctypes import wintypes

BCRYPT_ECDSA_PUBLIC_P256_MAGIC = 0x31534345  # "ECS1"
BCRYPT_ECDSA_PRIVATE_P256_MAGIC = 0x32534345  # "ECS2"
STATUS_INVALID_SIGNATURE = 0xC000A000
COORDINATE_BYTES = 32


class CngError(RuntimeError):
    pass


def _bcrypt():
    if sys.platform != "win32":
        raise CngError("Windows CNG (bcrypt.dll) is required for update signing.")
    library = ctypes.WinDLL("bcrypt.dll")
    library.BCryptOpenAlgorithmProvider.argtypes = [
        ctypes.POINTER(ctypes.c_void_p), wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.ULONG]
    library.BCryptCloseAlgorithmProvider.argtypes = [ctypes.c_void_p, wintypes.ULONG]
    library.BCryptGenerateKeyPair.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), wintypes.ULONG, wintypes.ULONG]
    library.BCryptFinalizeKeyPair.argtypes = [ctypes.c_void_p, wintypes.ULONG]
    library.BCryptImportKeyPair.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p),
        ctypes.c_void_p, wintypes.ULONG, wintypes.ULONG]
    library.BCryptExportKey.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.ULONG,
        ctypes.POINTER(wintypes.ULONG), wintypes.ULONG]
    library.BCryptSignHash.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p,
        wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG]
    library.BCryptVerifySignature.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p,
        wintypes.ULONG, wintypes.ULONG]
    library.BCryptDestroyKey.argtypes = [ctypes.c_void_p]
    for name in ("BCryptOpenAlgorithmProvider", "BCryptCloseAlgorithmProvider", "BCryptGenerateKeyPair",
                 "BCryptFinalizeKeyPair", "BCryptImportKeyPair", "BCryptExportKey", "BCryptSignHash",
                 "BCryptVerifySignature", "BCryptDestroyKey"):
        getattr(library, name).restype = ctypes.c_long
    return library


def _check(status: int, operation: str) -> None:
    if status < 0:
        raise CngError(f"{operation} failed (NTSTATUS 0x{status & 0xFFFFFFFF:08X}).")


class _Provider:
    def __enter__(self):
        self.library = _bcrypt()
        self.handle = ctypes.c_void_p()
        _check(self.library.BCryptOpenAlgorithmProvider(ctypes.byref(self.handle), "ECDSA_P256", None, 0),
               "BCryptOpenAlgorithmProvider")
        return self

    def __exit__(self, *_):
        self.library.BCryptCloseAlgorithmProvider(self.handle, 0)

    def import_key(self, x: bytes, y: bytes, d: bytes | None) -> ctypes.c_void_p:
        private = d is not None
        magic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC if private else BCRYPT_ECDSA_PUBLIC_P256_MAGIC
        blob = bytearray(magic.to_bytes(4, "little") + COORDINATE_BYTES.to_bytes(4, "little") + x + y + (d or b""))
        buffer = (ctypes.c_ubyte * len(blob)).from_buffer(blob)
        key = ctypes.c_void_p()
        try:
            _check(self.library.BCryptImportKeyPair(
                self.handle, None, "ECCPRIVATEBLOB" if private else "ECCPUBLICBLOB", ctypes.byref(key),
                buffer, len(blob), 0), "BCryptImportKeyPair")
        finally:
            ctypes.memset(buffer, 0, len(blob))
        return key


def _b64url_decode(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def _b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


class PrivateKey:
    def __init__(self, x: bytes, y: bytes, d: bytes):
        if len(x) != 32 or len(y) != 32 or len(d) != 32:
            raise CngError("A P-256 key needs 32-byte x, y and d values.")
        self.x, self.y, self._d = x, y, d

    @property
    def public_point(self) -> bytes:
        """Uncompressed SEC1 point (0x04 || X || Y), as p256_import_public expects."""
        return b"\x04" + self.x + self.y

    @classmethod
    def generate(cls) -> "PrivateKey":
        with _Provider() as provider:
            key = ctypes.c_void_p()
            _check(provider.library.BCryptGenerateKeyPair(provider.handle, ctypes.byref(key), 256, 0),
                   "BCryptGenerateKeyPair")
            try:
                _check(provider.library.BCryptFinalizeKeyPair(key, 0), "BCryptFinalizeKeyPair")
                blob = (ctypes.c_ubyte * 104)()
                written = wintypes.ULONG()
                _check(provider.library.BCryptExportKey(key, None, "ECCPRIVATEBLOB", blob, 104,
                                                        ctypes.byref(written), 0), "BCryptExportKey")
                raw = bytes(blob)
                ctypes.memset(blob, 0, 104)
            finally:
                provider.library.BCryptDestroyKey(key)
        if written.value != 104 or int.from_bytes(raw[0:4], "little") != BCRYPT_ECDSA_PRIVATE_P256_MAGIC:
            raise CngError("CNG exported an unexpected private key blob.")
        return cls(raw[8:40], raw[40:72], raw[72:104])

    @classmethod
    def from_jwk(cls, text: str) -> "PrivateKey":
        try:
            jwk = json.loads(text)
        except json.JSONDecodeError as error:
            raise CngError(f"The signing key is not valid JSON: {error.msg}.") from None
        if not isinstance(jwk, dict) or jwk.get("kty") != "EC" or jwk.get("crv") != "P-256" or "d" not in jwk:
            raise CngError('The signing key must be a private JWK with "kty": "EC" and "crv": "P-256".')
        return cls(_b64url_decode(jwk["x"]), _b64url_decode(jwk["y"]), _b64url_decode(jwk["d"]))

    def to_jwk(self) -> str:
        return json.dumps({"kty": "EC", "crv": "P-256", "x": _b64url_encode(self.x),
                           "y": _b64url_encode(self.y), "d": _b64url_encode(self._d)}, indent=2) + "\n"

    def sign_digest(self, digest: bytes) -> bytes:
        if len(digest) != 32:
            raise CngError("ECDSA P-256 signing expects a SHA-256 digest.")
        with _Provider() as provider:
            key = provider.import_key(self.x, self.y, self._d)
            try:
                signature = (ctypes.c_ubyte * 64)()
                written = wintypes.ULONG()
                digest_buffer = (ctypes.c_ubyte * 32).from_buffer_copy(digest)
                _check(provider.library.BCryptSignHash(key, None, digest_buffer, 32, signature, 64,
                                                       ctypes.byref(written), 0), "BCryptSignHash")
            finally:
                provider.library.BCryptDestroyKey(key)
        if written.value != 64:
            raise CngError("CNG returned a signature of unexpected length.")
        return bytes(signature)


def parse_public_point(text: str) -> bytes:
    """Accept 130 hex digits (optionally separated by spaces, colons or commas/0x)."""
    cleaned = text.strip().lower().replace("0x", "")
    for separator in (" ", ":", ",", "\n", "\r", "\t", "{", "}"):
        cleaned = cleaned.replace(separator, "")
    try:
        point = bytes.fromhex(cleaned)
    except ValueError:
        raise CngError("The public key must be hexadecimal.") from None
    if len(point) != 65 or point[0] != 4:
        raise CngError("The public key must be a 65-byte uncompressed SEC1 point (04 || X || Y).")
    return point


def verify_digest(public_point: bytes, digest: bytes, signature: bytes) -> bool:
    if len(public_point) != 65 or public_point[0] != 4 or len(digest) != 32 or len(signature) != 64:
        return False
    with _Provider() as provider:
        key = provider.import_key(public_point[1:33], public_point[33:65], None)
        try:
            digest_buffer = (ctypes.c_ubyte * 32).from_buffer_copy(digest)
            signature_buffer = (ctypes.c_ubyte * 64).from_buffer_copy(signature)
            status = provider.library.BCryptVerifySignature(key, None, digest_buffer, 32, signature_buffer, 64, 0)
        finally:
            provider.library.BCryptDestroyKey(key)
    if status >= 0:
        return True
    if status & 0xFFFFFFFF == STATUS_INVALID_SIGNATURE:
        return False
    _check(status, "BCryptVerifySignature")
    return False
