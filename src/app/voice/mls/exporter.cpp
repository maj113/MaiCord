#include "maicord/app/voice/mls/exporter.hpp"
#include "maicord/core/byte_order.hpp"
#include "../../shell/byte_views.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
// The exporter secret is 32 bytes and the output at most 8160 (the KDF checks it); every caller exports 16.
KdfResult mls_export_secret(KdfSha256Workspace& workspace, ConstSlice<u8> exporter_secret,
                            ConstSlice<u8> label, ConstSlice<u8> context, Slice<u8> output) noexcept {
    u8 secret[32], context_hash[32];
    auto result = mls_expand_label(workspace, exporter_secret, label, {}, {secret, 32});
    if (result.ok())
        result = sha256(workspace, {&context, 1}, {context_hash, 32});
    static constexpr u8 exported[]{'e', 'x', 'p', 'o', 'r', 't', 'e', 'd'};
    if (result.ok())
        result = mls_expand_label(workspace, {secret, 32}, {exported, sizeof(exported)}, {context_hash, 32},
                                  output);
    secure_clear_bytes(secret, sizeof(secret));
    secure_clear_bytes(context_hash, sizeof(context_hash));
    if (!result.ok())
        secure_clear_bytes(output.data, output.count);
    return result;
}
KdfResult dave_export_media_seed(KdfSha256Workspace& workspace, ConstSlice<u8> exporter_secret, u64 user_id,
                                 Slice<u8> output) noexcept {
    if (!user_id)
        return {0, 0, KdfError::invalid_input};
    u8 context[8];
    byte_store_le64(context, user_id);
    const auto result = mls_export_secret(workspace, exporter_secret,
                                          text_bytes("Discord Secure Frames v0"), {context, 8},
                                          {output.data, 16});
    secure_clear_bytes(context, sizeof(context));
    return result;
}
} // namespace maicord
