#include "maicord/app/voice/mls/hpke.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/voice/mls_encoding.hpp"
#include "../../shell/byte_views.hpp"

namespace maicord {
namespace {
struct Framing {
    u8 label_length[4], context_length[4];
    ConstSlice<u8> parts[5];
};
void frame(Framing& framing, ConstSlice<u8> label, ConstSlice<u8> context) noexcept {
    static constexpr u8 prefix[]{'M', 'L', 'S', ' ', '1', '.', '0', ' '};
    const u32 nl = mls_vector_length(label.count + 8, framing.label_length);
    const u32 nc = mls_vector_length(context.count, framing.context_length);
    framing.parts[0] = {framing.label_length, nl};
    framing.parts[1] = {prefix, 8};
    framing.parts[2] = label;
    framing.parts[3] = {framing.context_length, nc};
    framing.parts[4] = context;
}
// One step's outcome, whichever result type reports it.
struct Step {
    bool ok;
    i32 status;
};
template <typename Result> Step step(const Result& result) noexcept {
    return {result.ok(), result.native_status};
}
// The key schedule of the framed info over `shared`, and the one AEAD operation of the message.
HpkeResult crypt(KdfSha256Workspace& workspace, const Framing& framing, const u8* shared, ConstSlice<u8> input,
                 Slice<u8> output, bool seal) noexcept {
    u8 info_hash[32];
    HpkeBaseSecrets secrets;
    AesGcmKey cipher{};
    AeadResult aead{};
    const u32 payload = seal ? input.count : input.count - 16;
    auto done = step(hpke_info_hash(workspace, {framing.parts, 5}, {info_hash, 32}));
    if (done.ok)
        done = step(hpke_base_key_schedule_hashed_info(workspace, {shared, 32}, {info_hash, 32}, &secrets));
    if (done.ok)
        done = step(aes_gcm_initialize(&cipher, {secrets.key, 16}));
    if (done.ok) {
        aead = seal ? aes_gcm_encrypt(cipher, {secrets.base_nonce, 12}, {}, input, {output.data, payload},
                                      {output.data + payload, 16})
                    : aes_gcm_decrypt(cipher, {secrets.base_nonce, 12}, {}, {input.data, payload},
                                      {input.data + payload, 16}, {output.data, payload});
        done = step(aead);
    }
    aes_gcm_release(&cipher);
    hpke_base_secrets_clear(&secrets);
    secure_clear_bytes(info_hash, 32);
    if (done.ok)
        return {seal ? payload + 16 : payload, 0, HpkeError::none};
    return {0, done.status,
            aead.error == AeadError::authentication_failed ? HpkeError::authentication_failed : HpkeError::failure};
}
} // namespace
HpkeResult mls_encrypt_with_label(KdfSha256Workspace& workspace, ConstSlice<u8> recipient,
                                  ConstSlice<u8> label, ConstSlice<u8> context, ConstSlice<u8> plaintext,
                                  Slice<u8> encapsulated, Slice<u8> ciphertext) noexcept {
    Framing framing;
    frame(framing, label, context);
    P256Key ephemeral{};
    u8 enc[65], shared[32];
    // A fresh ephemeral key for every encapsulation.
    auto done = step(p256_generate(&ephemeral, P256Kind::ecdh));
    if (done.ok)
        done = step(hpke_encap(workspace, ephemeral, recipient, {enc, 65}, {shared, 32}));
    p256_release(&ephemeral);
    HpkeResult result{0, done.status, HpkeError::failure};
    if (done.ok)
        result = crypt(workspace, framing, shared, plaintext, ciphertext, true);
    if (result.ok())
        copy_bytes(encapsulated.data, enc, 65);
    secure_clear_bytes(shared, 32);
    return result;
}
HpkeResult mls_decrypt_with_label(KdfSha256Workspace& workspace, const P256Key& recipient,
                                  ConstSlice<u8> label, ConstSlice<u8> context, ConstSlice<u8> encapsulated,
                                  ConstSlice<u8> ciphertext, Slice<u8> plaintext) noexcept {
    Framing framing;
    frame(framing, label, context);
    u8 shared[32];
    const auto decapsulated = hpke_decap(workspace, recipient, encapsulated, {shared, 32});
    HpkeResult result{0, decapsulated.native_status, HpkeError::failure};
    // The ciphertext comes from the network: it must hold the tag, and its plaintext must fit.
    if (decapsulated.ok() && ciphertext.count >= 16 && plaintext.count >= ciphertext.count - 16)
        result = crypt(workspace, framing, shared, ciphertext, plaintext, false);
    secure_clear_bytes(shared, 32);
    return result;
}
} // namespace maicord
