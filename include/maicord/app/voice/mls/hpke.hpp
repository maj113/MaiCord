#pragma once
#include "maicord/app/voice/mls/hpke_derivation.hpp"
#include "maicord/platform/crypto/aes_gcm.hpp"

namespace maicord {
enum class HpkeError : u8 {
    none,
    failure,
    authentication_failed
};
struct HpkeResult {
    u32 bytes;
    i32 native_status;
    HpkeError error;
    constexpr bool ok() const noexcept {
        return error == HpkeError::none;
    }
};
// MLS ciphersuite 2 EncryptWithLabel/DecryptWithLabel over HPKE base mode, P-256/SHA256/AES128-GCM: one message
// per HPKE context (sequence 0, empty AAD). Canonical EncryptContext framing is streamed into HPKE info. No
// sender authentication is implied: MLS must bind keys, labels, context and encrypted objects itself.
// Encryption returns the ciphertext length INCLUDING the 16-byte tag; the 65-byte encapsulation is a separate
// output, written only after successful encryption. The callers (all inside the MLS group code) pass 65-byte
// recipient points and encapsulations, an output of at least the plaintext length plus 16 for encryption and a
// ciphertext of at least 16 bytes into an output of its length minus 16 for decryption; the buffers are
// distinct, or the plaintext and ciphertext of one message coincide exactly. AEAD failure erases the attempted
// payload (and seal tag).
HpkeResult mls_encrypt_with_label(KdfSha256Workspace& workspace, ConstSlice<u8> recipient,
                                  ConstSlice<u8> label, ConstSlice<u8> context, ConstSlice<u8> plaintext,
                                  Slice<u8> encapsulated, Slice<u8> ciphertext) noexcept;
HpkeResult mls_decrypt_with_label(KdfSha256Workspace& workspace, const P256Key& recipient,
                                  ConstSlice<u8> label, ConstSlice<u8> context, ConstSlice<u8> encapsulated,
                                  ConstSlice<u8> ciphertext, Slice<u8> plaintext) noexcept;
} // namespace maicord
