"""CNG validation probe using synthetic P-256 private scalar 1 only."""
import ctypes as c
import json
import struct
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

b = c.WinDLL('bcrypt')
b.BCryptImportKeyPair.argtypes = [c.c_void_p, c.c_void_p, c.c_wchar_p, c.POINTER(c.c_void_p), c.c_void_p, c.c_ulong, c.c_ulong]
b.BCryptImportKeyPair.restype = c.c_long
b.BCryptExportKey.argtypes = [c.c_void_p, c.c_void_p, c.c_wchar_p, c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong), c.c_ulong]
b.BCryptExportKey.restype = c.c_long
b.BCryptDestroyKey.argtypes = [c.c_void_p]
public = lambda n: ec.derive_private_key(n, ec.SECP256R1()).public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)[1:]
for name, coordinates in [('valid', public(1)), ('mismatched_public', public(2)), ('zero_public', bytes(64)), ('invalid_public', bytes([255])*64)]:
    blob = c.create_string_buffer(struct.pack('<II', 0x324b4345, 32) + coordinates + (1).to_bytes(32, 'big'))
    key = c.c_void_p()
    status = b.BCryptImportKeyPair(c.c_void_p(0x2a1), None, 'ECCPRIVATEBLOB', c.byref(key), blob, 104, 0)
    result = {'case': name, 'status': f'{status & 0xffffffff:08x}'}
    if status >= 0:
        output = c.create_string_buffer(72)
        written = c.c_ulong()
        exported = b.BCryptExportKey(key, None, 'ECCPUBLICBLOB', output, 72, c.byref(written), 0)
        result.update(export_status=f'{exported & 0xffffffff:08x}', public_matches_scalar=output.raw[8:72] == public(1))
    if key: b.BCryptDestroyKey(key)
    print(json.dumps(result))
