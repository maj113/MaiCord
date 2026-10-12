"""Read CNG tag capabilities and test empty GCM calls with synthetic zero keys.

Windows QA only. Does not read client memory, credentials, or account state.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import platform


class Auth(c.Structure):
    _fields_ = [("size", w.ULONG), ("version", w.ULONG),
                ("nonce", c.c_void_p), ("nonce_bytes", w.ULONG),
                ("aad", c.c_void_p), ("aad_bytes", w.ULONG),
                ("tag", c.c_void_p), ("tag_bytes", w.ULONG),
                ("mac", c.c_void_p), ("mac_bytes", w.ULONG),
                ("total_aad", w.ULONG), ("total_data", c.c_ulonglong),
                ("flags", w.ULONG)]


def main():
    b = c.WinDLL('bcrypt')
    signatures = {
        'BCryptOpenAlgorithmProvider': [c.POINTER(c.c_void_p), w.LPCWSTR, w.LPCWSTR, w.ULONG],
        'BCryptSetProperty': [c.c_void_p, w.LPCWSTR, c.c_void_p, w.ULONG, w.ULONG],
        'BCryptGetProperty': [c.c_void_p, w.LPCWSTR, c.c_void_p, w.ULONG, c.POINTER(w.ULONG), w.ULONG],
        'BCryptGenerateSymmetricKey': [c.c_void_p, c.POINTER(c.c_void_p), c.c_void_p, w.ULONG,
                                      c.c_void_p, w.ULONG, w.ULONG],
        'BCryptDestroyKey': [c.c_void_p],
        'BCryptCloseAlgorithmProvider': [c.c_void_p, w.ULONG],
    }
    crypt_signature = [c.c_void_p, c.c_void_p, w.ULONG, c.c_void_p, c.c_void_p, w.ULONG,
                       c.c_void_p, w.ULONG, c.POINTER(w.ULONG), w.ULONG]
    signatures['BCryptEncrypt'] = signatures['BCryptDecrypt'] = crypt_signature
    for name, args in signatures.items():
        getattr(b, name).argtypes = args
        getattr(b, name).restype = c.c_long
    def checked(status):
        if status < 0:
            raise RuntimeError(f'CNG setup failed: {status & 0xffffffff:08x}')
    provider = c.c_void_p()
    checked(b.BCryptOpenAlgorithmProvider(c.byref(provider), 'AES', None, 0))
    try:
        mode = c.create_unicode_buffer('ChainingModeGCM')
        checked(b.BCryptSetProperty(provider, 'ChainingMode', mode, c.sizeof(mode), 0))
        limits = (w.ULONG * 3)()
        returned = w.ULONG()
        checked(b.BCryptGetProperty(provider, 'AuthTagLength', limits, c.sizeof(limits), c.byref(returned), 0))
        print(json.dumps({'windows': platform.version(), 'tag_min_max_increment': list(limits),
                          'property_bytes': returned.value}))
        for tag_bytes in (8, 12, 16):
            key = c.c_void_p()
            secret = (c.c_ubyte * 16)()
            checked(b.BCryptGenerateSymmetricKey(provider, c.byref(key), None, 0, secret, 16, 0))
            try:
                nonce, tag = (c.c_ubyte * 12)(), (c.c_ubyte * tag_bytes)()
                auth = Auth(size=c.sizeof(Auth), version=1, nonce=c.addressof(nonce), nonce_bytes=12,
                            tag=c.addressof(tag), tag_bytes=tag_bytes)
                written = w.ULONG()
                enc = b.BCryptEncrypt(key, None, 0, c.byref(auth), None, 0, None, 0, c.byref(written), 0)
                dec = b.BCryptDecrypt(key, None, 0, c.byref(auth), None, 0, None, 0, c.byref(written), 0)
                print(json.dumps({'tag_bytes': tag_bytes, 'encrypt_status': f'{enc & 0xffffffff:08x}',
                                  'decrypt_status': f'{dec & 0xffffffff:08x}'}))
            finally:
                checked(b.BCryptDestroyKey(key))
    finally:
        checked(b.BCryptCloseAlgorithmProvider(provider, 0))


if __name__ == '__main__':
    main()
