#include "maicord/app/mls_hpke.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/mls_encoding.hpp"

namespace maicord {
namespace {
template <typename T> ConstSlice<u8> bytes(const T& value) noexcept {
    return {reinterpret_cast<const u8*>(&value), sizeof(value)};
}
HpkeResult preflight(const KdfSha256Workspace& workspace, ConstSlice<u8> point, ConstSlice<u8> label,
                     ConstSlice<u8> context, ConstSlice<u8> payload, Slice<u8> output, Slice<u8> enc,
                     bool encrypt) noexcept {
    if (!workspace.storage.data || !workspace.storage.locked)
        return {0, 0, HpkeError::not_initialized};
    if (point.count != 65 || label.count > 0x3fffffff - 8 || context.count > 0x3fffffff ||
        (encrypt ? payload.count > ~u32{0} - 16 : payload.count < 16))
        return {0, 0, HpkeError::invalid_input};
    const ConstSlice<u8> spans[]{
        point, label, context, payload, {output.data, output.count}, {enc.data, enc.count}};
    for (auto span : spans)
        if (!byte_range_valid(span) || byte_ranges_overlap(span, bytes(workspace)) ||
            byte_ranges_overlap(span, {workspace.storage.data, workspace.storage.capacity}))
            return {0, 0, HpkeError::invalid_input};
    const u32 wanted = encrypt ? payload.count + 16 : payload.count - 16;
    if (output.count < wanted || (encrypt && enc.count < 65))
        return {0, 0, HpkeError::output_capacity};
    const ConstSlice<u8> destination{output.data, wanted}, encapsulation{enc.data, encrypt ? 65u : 0u};
    if (byte_ranges_overlap(destination, point) || byte_ranges_overlap(destination, label) ||
        byte_ranges_overlap(destination, context) || byte_ranges_overlap(destination, encapsulation) ||
        (payload.data != output.data && byte_ranges_overlap(destination, payload)))
        return {0, 0, HpkeError::invalid_input};
    for (u32 i = 0; i != 4; ++i)
        if (byte_ranges_overlap(encapsulation, spans[i]))
            return {0, 0, HpkeError::invalid_input};
    return {};
}
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
} // namespace
HpkeResult mls_encrypt_with_label(KdfSha256Workspace& workspace, ConstSlice<u8> recipient,
                                  ConstSlice<u8> label, ConstSlice<u8> context, ConstSlice<u8> plaintext,
                                  Slice<u8> encapsulated, Slice<u8> ciphertext) noexcept {
    auto result =
        preflight(workspace, recipient, label, context, plaintext, ciphertext, encapsulated, true);
    if (!result.ok())
        return result;
    Framing framing;
    frame(framing, label, context);
    HpkeContext hpke{};
    u8 enc[65];
    result = hpke_setup_sender_parts(&hpke, workspace, recipient, {framing.parts, 5}, {enc, 65});
    if (result.ok())
        result = hpke_seal(hpke, {}, plaintext, ciphertext);
    if (result.ok())
        copy_bytes(encapsulated.data, enc, 65);
    hpke_release(&hpke);
    return result;
}
HpkeResult mls_decrypt_with_label(KdfSha256Workspace& workspace, const P256Key& recipient,
                                  ConstSlice<u8> label, ConstSlice<u8> context, ConstSlice<u8> encapsulated,
                                  ConstSlice<u8> ciphertext, Slice<u8> plaintext) noexcept {
    auto result = preflight(workspace, encapsulated, label, context, ciphertext, plaintext, {}, false);
    if (!result.ok())
        return result;
    const ConstSlice<u8> spans[]{label,
                                 context,
                                 encapsulated,
                                 ciphertext,
                                 {plaintext.data, plaintext.count},
                                 bytes(workspace),
                                 {workspace.storage.data, workspace.storage.capacity}};
    for (auto span : spans)
        if (byte_ranges_overlap(span, bytes(recipient)))
            return {0, 0, HpkeError::invalid_input};
    Framing framing;
    frame(framing, label, context);
    HpkeContext hpke{};
    result = hpke_setup_recipient_parts(&hpke, workspace, recipient, encapsulated, {framing.parts, 5});
    if (result.ok())
        result = hpke_open(hpke, {}, ciphertext, plaintext);
    hpke_release(&hpke);
    return result;
}
} // namespace maicord
