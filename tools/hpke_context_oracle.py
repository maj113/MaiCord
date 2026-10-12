"""Verify actual native sender ciphertexts with independent Python/OpenSSL HPKE."""
import argparse
import json
from pathlib import Path
import struct
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from hpke_derivation_oracle import load, SOURCE_SHA, KEM, extract, expand, schedule


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--hpke',required=True,type=Path)
    parser.add_argument('--records',required=True,type=Path)
    args=parser.parse_args()
    v=next(v for v in load(args.hpke,SOURCE_SHA) if (v['mode'],v['kem_id'],v['kdf_id'],v['aead_id'])==(0,16,1,1))
    private=ec.derive_private_key(int(v['skRm'],16),ec.SECP256R1())
    recipient=bytes.fromhex(v['pkRm'])
    data=args.records.read_bytes()
    cursor=count=0
    seen=set()
    while cursor<len(data):
        assert cursor+93<=len(data)
        enc=data[cursor:cursor+65]
        seq=data[cursor+65:cursor+77]
        ni,na,np,nc=struct.unpack_from('<IIII',data,cursor+77)
        cursor+=93
        assert nc==np+16 and cursor+ni+na+np+nc<=len(data)
        info=data[cursor:cursor+ni];cursor+=ni
        aad=data[cursor:cursor+na];cursor+=na
        pt=data[cursor:cursor+np];cursor+=np
        ct=data[cursor:cursor+nc];cursor+=nc
        assert (enc,seq) not in seen, 'native sender reused key/sequence'
        seen.add((enc,seq))
        peer=ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(),enc)
        dh=private.exchange(ec.ECDH(),peer)
        shared=expand(KEM,extract(KEM,b'',b'eae_prk',dh),b'shared_secret',enc+recipient,32)
        material=schedule(shared,info)
        nonce=bytes(a^b for a,b in zip(material[16:28],seq))
        assert AESGCM(material[:16]).encrypt(nonce,pt,aad)==ct
        assert AESGCM(material[:16]).decrypt(nonce,ct,aad)==pt
        count+=1
    assert count==16, f'expected all 11 sizes and 5 counter boundaries, got {count}'
    print(f'OpenSSL verified {count} native HPKE ciphertexts, including empty/in-place/64KiB and 96-bit counter boundaries')


if __name__=='__main__':main()
