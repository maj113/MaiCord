#pragma once
#include "maicord/app/hpke_context.hpp"

namespace maicord {
// MLS ciphersuite 2 EncryptWithLabel/DecryptWithLabel. Canonical EncryptContext
// framing is streamed into HPKE info; AEAD AAD is empty and each call uses one
// fresh local HPKE context, released on every exit. No sender authentication is
// implied: MLS must bind keys, labels, context and encrypted objects itself.
// Encryption returns ciphertext length INCLUDING the 16-byte tag; encapsulation
// is a separate 65-byte output, written only after successful encryption.
// Preflight/setup failures preserve output. AEAD failure erases the attempted
// payload (and seal tag); excess capacity is untouched. In-place payload
// operation is supported; all other output/input aliases are rejected.
HpkeResult mls_encrypt_with_label(KdfSha256Workspace& workspace, ConstSlice<u8> recipient,
                                  ConstSlice<u8> label, ConstSlice<u8> context, ConstSlice<u8> plaintext,
                                  Slice<u8> encapsulated, Slice<u8> ciphertext) noexcept;
HpkeResult mls_decrypt_with_label(KdfSha256Workspace& workspace, const P256Key& recipient,
                                  ConstSlice<u8> label, ConstSlice<u8> context, ConstSlice<u8> encapsulated,
                                  ConstSlice<u8> ciphertext, Slice<u8> plaintext) noexcept;
} // namespace maicord
