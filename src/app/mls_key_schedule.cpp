#include "maicord/app/mls_key_schedule.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u8 zero[32]{};
KdfResult validate(KdfSha256Workspace& workspace, ConstSlice<ConstSlice<u8>> inputs,
                   MlsEpochSecrets* output) noexcept {
    if (!workspace.storage.data || !workspace.storage.locked)
        return {0, 0, KdfError::not_initialized};
    const ConstSlice<u8> destination{reinterpret_cast<const u8*>(output), sizeof(*output)};
    const ConstSlice<u8> owner{reinterpret_cast<const u8*>(&workspace), sizeof(workspace)};
    const ConstSlice<u8> storage{workspace.storage.data, workspace.storage.capacity};
    if (!byte_range_valid(destination) || byte_ranges_overlap(destination, owner) ||
        byte_ranges_overlap(destination, storage))
        return {0, 0, KdfError::invalid_input};
    for (u32 i = 0; i != inputs.count; ++i)
        if (!byte_range_valid(inputs.data[i]) || byte_ranges_overlap(inputs.data[i], owner) ||
            byte_ranges_overlap(inputs.data[i], storage))
            return {0, 0, KdfError::invalid_input};
    return {};
}
template <u32 N>
KdfResult expand(KdfSha256Workspace& workspace, ConstSlice<u8> secret, const char (&label)[N],
                 ConstSlice<u8> context, u8* output) noexcept {
    return mls_expand_label(workspace, secret, {reinterpret_cast<const u8*>(label), N - 1}, context,
                            {output, 32});
}
KdfResult finish(KdfSha256Workspace& workspace, MlsEpochSecrets& next, ConstSlice<u8> psk,
                 ConstSlice<u8> confirmed, ConstSlice<u8> context) noexcept {
    u8 member[32];
    auto result = hkdf_sha256_extract(workspace, {next.joiner_secret, 32}, psk, {member, 32});
    if (result.ok())
        result = expand(workspace, {member, 32}, "welcome", {}, next.welcome_secret);
    if (result.ok())
        result = expand(workspace, {member, 32}, "epoch", context, next.epoch_secret);
    SecureZeroMemory(member, sizeof(member));
    struct Label {
        const char* text;
        u32 length;
        u8* output;
    };
    const Label labels[]{{"sender data", 11, next.sender_data_secret},
                         {"encryption", 10, next.encryption_secret},
                         {"exporter", 8, next.exporter_secret},
                         {"authentication", 14, next.epoch_authenticator},
                         {"external", 8, next.external_secret},
                         {"confirm", 7, next.confirmation_key},
                         {"membership", 10, next.membership_key},
                         {"resumption", 10, next.resumption_psk},
                         {"init", 4, next.init_secret}};
    for (const auto& label : labels) {
        if (!result.ok())
            break;
        result = mls_expand_label(workspace, {next.epoch_secret, 32},
                                  {reinterpret_cast<const u8*>(label.text), label.length}, {},
                                  {label.output, 32});
    }
    if (result.ok())
        result = hmac_sha256(workspace, {next.confirmation_key, 32}, {&confirmed, 1},
                             {next.confirmation_tag, 32});
    return result;
}
KdfResult derive(KdfSha256Workspace& workspace, ConstSlice<u8> initial, ConstSlice<u8> commit,
                 ConstSlice<u8> psk, ConstSlice<u8> confirmed, ConstSlice<u8> context,
                 MlsEpochSecrets* output, bool from_joiner) noexcept {
    if (initial.count != 32 || psk.count != 32 || (!from_joiner && commit.count != 32) ||
        (confirmed.count != 0 && confirmed.count != 32) || context.count > 0x3fffffff)
        return {0, 0, KdfError::invalid_input};
    const ConstSlice<u8> inputs[]{initial, commit, psk, confirmed, context};
    auto result = validate(workspace, {inputs, 5}, output);
    if (!result.ok())
        return result;
    MlsEpochSecrets next{};
    if (from_joiner) {
        copy_bytes(next.joiner_secret, initial.data, 32);
    } else {
        u8 pre_joiner[32];
        result = hkdf_sha256_extract(workspace, initial, commit, {pre_joiner, 32});
        if (result.ok())
            result = expand(workspace, {pre_joiner, 32}, "joiner", context, next.joiner_secret);
        SecureZeroMemory(pre_joiner, sizeof(pre_joiner));
    }
    if (result.ok())
        result = finish(workspace, next, psk, confirmed, context);
    if (result.ok()) {
        copy_bytes(output, &next, sizeof(next));
        result.bytes = sizeof(next);
    }
    mls_epoch_clear(&next);
    return result;
}
} // namespace
void mls_epoch_clear(MlsEpochSecrets* epoch) noexcept {
    if (epoch)
        SecureZeroMemory(epoch, sizeof(*epoch));
}
KdfResult mls_epoch_initial(KdfSha256Workspace& workspace, ConstSlice<u8> init_secret,
                            ConstSlice<u8> context, MlsEpochSecrets* output) noexcept {
    return derive(workspace, init_secret, {zero, 32}, {zero, 32}, {}, context, output, false);
}
KdfResult mls_epoch_next(KdfSha256Workspace& workspace, ConstSlice<u8> init_secret,
                         ConstSlice<u8> commit_secret, ConstSlice<u8> psk_secret,
                         ConstSlice<u8> confirmed_hash, ConstSlice<u8> context,
                         MlsEpochSecrets* output) noexcept {
    if (confirmed_hash.count != 32)
        return {0, 0, KdfError::invalid_input};
    return derive(workspace, init_secret, commit_secret, psk_secret, confirmed_hash, context, output,
                  false);
}
KdfResult mls_epoch_from_joiner(KdfSha256Workspace& workspace, ConstSlice<u8> joiner_secret,
                                ConstSlice<u8> psk_secret, ConstSlice<u8> confirmed_hash,
                                ConstSlice<u8> context, MlsEpochSecrets* output) noexcept {
    return derive(workspace, joiner_secret, {}, psk_secret, confirmed_hash, context, output, true);
}
KdfResult mls_transcript_confirm(KdfSha256Workspace& workspace, ConstSlice<u8> previous_interim,
                                 ConstSlice<u8> confirmed_input, Slice<u8> output) noexcept {
    if (previous_interim.count != 32)
        return {0, 0, KdfError::invalid_input};
    const ConstSlice<u8> parts[]{previous_interim, confirmed_input};
    return sha256(workspace, {parts, 2}, output);
}
KdfResult mls_transcript_interim(KdfSha256Workspace& workspace, ConstSlice<u8> confirmed_hash,
                                 ConstSlice<u8> confirmation_tag, Slice<u8> output) noexcept {
    if ((confirmed_hash.count != 0 && confirmed_hash.count != 32) || confirmation_tag.count != 32)
        return {0, 0, KdfError::invalid_input};
    constexpr u8 length = 32;
    const ConstSlice<u8> parts[]{confirmed_hash, {&length, 1}, confirmation_tag};
    return sha256(workspace, {parts, 3}, output);
}
bool mls_confirmation_matches(ConstSlice<u8> expected, ConstSlice<u8> received) noexcept {
    if (expected.count != 32 || received.count != 32 || !byte_range_valid(expected) ||
        !byte_range_valid(received))
        return false;
    const volatile u8* left = expected.data;
    const volatile u8* right = received.data;
    u32 difference = 0;
    for (u32 i = 0; i != 32; ++i)
        difference |= left[i] ^ right[i];
    return difference == 0;
}
} // namespace maicord
