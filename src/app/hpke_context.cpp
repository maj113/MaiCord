#include "maicord/app/hpke_context.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "byte_views.hpp"


namespace maicord {
namespace {
bool initialized(const HpkeContext& context) noexcept {
    return (context.role == HpkeRole::sender || context.role == HpkeRole::recipient) && context.cipher.handle;
}
HpkeResult setup_preflight(const HpkeContext* context, const KdfSha256Workspace& workspace,
                           ConstSlice<u8> point, ConstSlice<ConstSlice<u8>> info, ConstSlice<u8> output,
                           bool sender) noexcept {
    if (!context)
        return {0, 0, HpkeError::invalid_input};
    if (!workspace.storage.data || !workspace.storage.locked)
        return {0, 0, HpkeError::not_initialized};
    if (hpke_aliases(*context, object_bytes(workspace)) ||
        hpke_aliases(*context, {workspace.storage.data, workspace.storage.capacity}))
        return {0, 0, HpkeError::invalid_input};
    if (info.count > ~u32{0} / sizeof(ConstSlice<u8>))
        return {0, 0, HpkeError::invalid_input};
    const ConstSlice<u8> descriptors{reinterpret_cast<const u8*>(info.data),
                                     info.count * static_cast<u32>(sizeof(ConstSlice<u8>))};
    const ConstSlice<u8> spans[]{point, descriptors, output};
    for (auto span : spans)
        if (!byte_range_valid(span) || hpke_aliases(*context, span) || aliases_workspace(workspace, span))
            return {0, 0, HpkeError::invalid_input};
    if (point.count != 65 || byte_ranges_overlap(output, point) || byte_ranges_overlap(output, descriptors))
        return {0, 0, HpkeError::invalid_input};
    for (u32 i = 0; i != info.count; ++i) {
        const auto span = info.data[i];
        if (!byte_range_valid(span) || hpke_aliases(*context, span) || aliases_workspace(workspace, span) ||
            byte_ranges_overlap(output, span))
            return {0, 0, HpkeError::invalid_input};
    }
    if (sender && output.count < 65)
        return {0, 0, HpkeError::output_capacity};
    return {};
}
HpkeResult from_derivation(HpkeDerivationResult result) noexcept {
    return {result.bytes, result.native_status,
            result.ok() ? HpkeError::none : HpkeError::derivation_failure};
}
HpkeResult install(HpkeContext* context, KdfSha256Workspace& workspace, ConstSlice<u8> shared,
                   ConstSlice<u8> info_hash, HpkeRole role) noexcept {
    HpkeBaseSecrets material{};
    HpkeContext candidate{};
    auto result =
        from_derivation(hpke_base_key_schedule_hashed_info(workspace, shared, info_hash, &material));
    if (result.ok()) {
        const auto cipher = aes_gcm_initialize(&candidate.cipher, {material.key, 16});
        if (!cipher.ok())
            result = {0, cipher.native_status,
                      cipher.error == AeadError::storage ? HpkeError::storage_failure
                                                         : HpkeError::platform_failure};
        else {
            copy_bytes(candidate.base_nonce, material.base_nonce, 12);
            candidate.role = role;
            // Explicit ownership transfer: no key handle survives in both owners.
            HpkeContext previous = *context;
            *context = candidate;
            reset_value(&candidate);
            hpke_release(&previous);
            result.bytes = 0;
        }
    }
    hpke_base_secrets_clear(&material);
    hpke_release(&candidate);
    return result;
}
HpkeResult crypt(HpkeContext& context, ConstSlice<u8> aad, ConstSlice<u8> input, Slice<u8> output,
                 bool seal) noexcept {
    if (!initialized(context))
        return {0, 0, HpkeError::not_initialized};
    if (context.role != (seal ? HpkeRole::sender : HpkeRole::recipient))
        return {0, 0, HpkeError::wrong_role};
    if (context.poisoned)
        return {0, 0, HpkeError::poisoned};
    if (!byte_range_valid(aad) || !byte_range_valid(input) ||
        !byte_range_valid({output.data, output.count}) ||
        (seal ? input.count > ~u32{0} - 16 : input.count < 16))
        return {0, 0, HpkeError::invalid_input};
    const u32 payload = seal ? input.count : input.count - 16;
    const u32 wanted = seal ? payload + 16 : payload;
    if (output.count < wanted)
        return {0, 0, HpkeError::output_capacity};
    const ConstSlice<u8> destination{output.data, wanted};
    if (hpke_aliases(context, aad) || hpke_aliases(context, input) || hpke_aliases(context, destination) ||
        byte_ranges_overlap(aad, destination) ||
        (input.data != output.data && byte_ranges_overlap(input, destination)))
        return {0, 0, HpkeError::invalid_input};
    u8 all = 0xff;
    for (auto byte : context.sequence)
        all &= byte;
    if (all == 0xff)
        return {0, 0, HpkeError::sequence_exhausted};
    u8 nonce[12];
    for (u32 i = 0; i != 12; ++i)
        nonce[i] = context.base_nonce[i] ^ context.sequence[i];
    const auto result = seal ? aes_gcm_encrypt(context.cipher, {nonce, 12}, aad, input,
                                               {output.data, payload}, {output.data + payload, 16})
                             : aes_gcm_decrypt(context.cipher, {nonce, 12}, aad, {input.data, payload},
                                               {input.data + payload, 16}, {output.data, payload});
    secure_clear_bytes(nonce, 12);
    if (result.ok()) {
        for (u32 i = 12; i != 0;)
            if (++context.sequence[--i])
                break;
        return {wanted, 0, HpkeError::none};
    }
    if (result.error == AeadError::authentication_failed && !seal)
        return {0, result.native_status, HpkeError::authentication_failed};
    if (result.error == AeadError::platform_failure || result.error == AeadError::authentication_failed) {
        context.poisoned = true;
        return {0, result.native_status, HpkeError::platform_failure};
    }
    return {0, result.native_status, HpkeError::invalid_input};
}
} // namespace
void hpke_release(HpkeContext* context) noexcept {
    if (!context)
        return;
    aes_gcm_release(&context->cipher);
    reset_value(context);
}
bool hpke_aliases(const HpkeContext& context, ConstSlice<u8> span) noexcept {
    return byte_ranges_overlap(span, object_bytes(context)) ||
           byte_ranges_overlap(span, {context.cipher.object.data, context.cipher.object.capacity});
}
HpkeResult hpke_setup_sender_parts(HpkeContext* context, KdfSha256Workspace& workspace,
                                   ConstSlice<u8> recipient, ConstSlice<ConstSlice<u8>> info,
                                   Slice<u8> encapsulated) noexcept {
    auto result =
        setup_preflight(context, workspace, recipient, info, {encapsulated.data, encapsulated.count}, true);
    if (!result.ok())
        return result;
    P256Key ephemeral{};
    u8 shared[32], enc[65], info_hash[32];
    const auto hashed = hpke_info_hash(workspace, info, {info_hash, 32});
    if (!hashed.ok())
        result = {0, hashed.native_status, HpkeError::derivation_failure};
    else {
        const auto generated = p256_generate(&ephemeral, P256Kind::ecdh);
        if (!generated.ok())
            result = {0, generated.native_status, HpkeError::platform_failure};
        else
            result = from_derivation(hpke_encap(workspace, ephemeral, recipient, {enc, 65}, {shared, 32}));
    }
    p256_release(&ephemeral);
    if (result.ok())
        result = install(context, workspace, {shared, 32}, {info_hash, 32}, HpkeRole::sender);
    if (result.ok()) {
        copy_bytes(encapsulated.data, enc, 65);
        result.bytes = 65;
    }
    secure_clear_bytes(shared, 32);
    secure_clear_bytes(info_hash, 32);
    return result;
}
HpkeResult hpke_setup_recipient_parts(HpkeContext* context, KdfSha256Workspace& workspace,
                                      const P256Key& recipient, ConstSlice<u8> encapsulated,
                                      ConstSlice<ConstSlice<u8>> info) noexcept {
    if (info.count > ~u32{0} / sizeof(ConstSlice<u8>) ||
        byte_ranges_overlap(object_bytes(recipient), {reinterpret_cast<const u8*>(info.data),
                                               info.count * static_cast<u32>(sizeof(ConstSlice<u8>))}))
        return {0, 0, HpkeError::invalid_input};
    auto result = setup_preflight(context, workspace, encapsulated, info, {}, false);
    if (!result.ok())
        return result;
    if (hpke_aliases(*context, object_bytes(recipient)) ||
        aliases_workspace(workspace, object_bytes(recipient)) ||
        byte_ranges_overlap(object_bytes(recipient), encapsulated))
        return {0, 0, HpkeError::invalid_input};
    for (u32 i = 0; i != info.count; ++i)
        if (byte_ranges_overlap(object_bytes(recipient), info.data[i]))
            return {0, 0, HpkeError::invalid_input};
    u8 shared[32], info_hash[32];
    const auto hashed = hpke_info_hash(workspace, info, {info_hash, 32});
    if (!hashed.ok())
        result = {0, hashed.native_status, HpkeError::derivation_failure};
    else
        result = from_derivation(hpke_decap(workspace, recipient, encapsulated, {shared, 32}));
    if (result.ok())
        result = install(context, workspace, {shared, 32}, {info_hash, 32}, HpkeRole::recipient);
    secure_clear_bytes(shared, 32);
    secure_clear_bytes(info_hash, 32);
    return result;
}
HpkeResult hpke_seal(HpkeContext& context, ConstSlice<u8> aad, ConstSlice<u8> plaintext,
                     Slice<u8> ciphertext) noexcept {
    return crypt(context, aad, plaintext, ciphertext, true);
}
HpkeResult hpke_open(HpkeContext& context, ConstSlice<u8> aad, ConstSlice<u8> ciphertext,
                     Slice<u8> plaintext) noexcept {
    return crypt(context, aad, ciphertext, plaintext, false);
}
} // namespace maicord
