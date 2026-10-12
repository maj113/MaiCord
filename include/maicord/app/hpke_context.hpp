#pragma once
#include "maicord/app/hpke_derivation.hpp"
#include "maicord/platform/aes_gcm.hpp"

namespace maicord {
enum class HpkeRole : u8 {
    none,
    sender,
    recipient
};
enum class HpkeError : u8 {
    none,
    invalid_input,
    not_initialized,
    wrong_role,
    output_capacity,
    derivation_failure,
    storage_failure,
    platform_failure,
    authentication_failed,
    sequence_exhausted,
    poisoned
};
struct HpkeResult {
    u32 bytes;
    i32 native_status;
    HpkeError error;
    constexpr bool ok() const noexcept {
        return error == HpkeError::none;
    }
};
// Zero initialize, worker exclusive, no copies. Internal fields are not caller
// controls. A full 96-bit big-endian sequence avoids an artificial 64-bit limit.
// The exporter secret lives in the cipher's locked auxiliary storage; only
// non-secret nonce/counter and ownership metadata live in this object.
struct HpkeContext {
    AesGcmKey cipher;
    u8 base_nonce[12], sequence[12];
    HpkeRole role;
    bool poisoned;
};
void hpke_release(HpkeContext* context) noexcept;
bool hpke_aliases(const HpkeContext& context, ConstSlice<u8> span) noexcept;
// Base mode, P-256/SHA256/AES128-GCM. Sender setup generates and retires a fresh
// ephemeral key. Both setups stage all ownership/material before replacing a
// live context; ALL setup failures preserve it and the encapsulation output.
// Inputs/output must avoid the old context, cipher allocation and KDF workspace.
HpkeResult hpke_setup_sender(HpkeContext* context, KdfSha256Workspace& workspace, ConstSlice<u8> recipient,
                             ConstSlice<u8> info_a, ConstSlice<u8> info_b, Slice<u8> encapsulated) noexcept;
HpkeResult hpke_setup_recipient(HpkeContext* context, KdfSha256Workspace& workspace,
                                const P256Key& recipient, ConstSlice<u8> encapsulated,
                                ConstSlice<u8> info_a, ConstSlice<u8> info_b) noexcept;
// Arbitrarily segmented setup info, with the same ownership/alias rules.
// Descriptor storage is borrowed and validated before reading its elements.
HpkeResult hpke_setup_sender_parts(HpkeContext* context, KdfSha256Workspace& workspace,
                                   ConstSlice<u8> recipient, ConstSlice<ConstSlice<u8>> info,
                                   Slice<u8> encapsulated) noexcept;
HpkeResult hpke_setup_recipient_parts(HpkeContext* context, KdfSha256Workspace& workspace,
                                      const P256Key& recipient, ConstSlice<u8> encapsulated,
                                      ConstSlice<ConstSlice<u8>> info) noexcept;
// Ciphertext includes the 16-byte tag. Exact in-place payload operation is
// supported, partial overlap rejected. Validation failures preserve output.
// Native failure wipes the attempted output; a bad tag leaves the receive
// sequence unchanged for a retry. Other native failures poison the context
// until fresh setup. Success alone advances the sequence. The terminal counter
// is rejected before AEAD, because its increment would overflow.
HpkeResult hpke_seal(HpkeContext& context, ConstSlice<u8> aad, ConstSlice<u8> plaintext,
                     Slice<u8> ciphertext) noexcept;
HpkeResult hpke_open(HpkeContext& context, ConstSlice<u8> aad, ConstSlice<u8> ciphertext,
                     Slice<u8> plaintext) noexcept;
// Export does not advance or depend on AEAD sequence state. Requested length
// is output.count; native KDF failure wipes it. Callers must validate outer
// protocol identity, ordering and replay policy; Base HPKE does not authenticate
// the sender or authorize MLS membership.
HpkeResult hpke_export(const HpkeContext& context, KdfSha256Workspace& workspace,
                       ConstSlice<u8> exporter_context, Slice<u8> output) noexcept;
} // namespace maicord
