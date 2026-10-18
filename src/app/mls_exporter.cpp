#include "maicord/app/mls_exporter.hpp"
#include "maicord/core/byte_range.hpp"
#include "byte_views.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
KdfResult mls_export_secret(KdfSha256Workspace& workspace, ConstSlice<u8> exporter_secret,
                            ConstSlice<u8> label, ConstSlice<u8> context, Slice<u8> output) noexcept {
    if (!workspace.storage.data || !workspace.storage.locked)
        return {0, 0, KdfError::not_initialized};
    if (exporter_secret.count != 32 || label.count > 0x3fffffff - 8 || output.count > 8160 ||
        !byte_range_valid({output.data, output.count}))
        return {0, 0, KdfError::invalid_input};
    const ConstSlice<u8> destination{output.data, output.count};
    if (aliases_workspace(workspace, destination))
        return {0, 0, KdfError::invalid_input};
    const ConstSlice<u8> inputs[]{exporter_secret, label, context};
    for (auto input : inputs)
        if (!byte_range_valid(input) || byte_ranges_overlap(input, destination) ||
            aliases_workspace(workspace, input))
            return {0, 0, KdfError::invalid_input};
    if (!output.count)
        return {};
    u8 secret[32], context_hash[32];
    auto result = mls_expand_label(workspace, exporter_secret, label, {}, {secret, 32});
    if (result.ok())
        result = sha256(workspace, {&context, 1}, {context_hash, 32});
    constexpr u8 exported[]{'e', 'x', 'p', 'o', 'r', 't', 'e', 'd'};
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
    if (!user_id || !byte_range_valid({output.data, output.count}))
        return {0, 0, KdfError::invalid_input};
    if (output.count < 16)
        return {0, 0, KdfError::output_capacity};
    u8 context[8];
    for (u32 i = 0; i != 8; ++i)
        context[i] = static_cast<u8>(user_id >> (i * 8));
    const auto result = mls_export_secret(workspace, exporter_secret,
                                          text_bytes("Discord Secure Frames v0"), {context, 8},
                                          {output.data, 16});
    secure_clear_bytes(context, sizeof(context));
    return result;
}
} // namespace maicord
